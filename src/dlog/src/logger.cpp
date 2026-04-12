// =============================================================================
// logger.cpp — dlog 日志系统实现
//
// 架构：
//   写日志线程（任意）           后台 flush 线程
//   ─────────────────            ───────────────
//   log() → 格式化              每 N 秒唤醒一次
//         → 立即打印 stderr     → 把 pending_ swap 出来
//         → push pending_       → 用 popen("gzip -c >") 写 .log.gz
//
// 文件命名：dlog_20260412_102536_0.log.gz
//           （末尾序号防止同秒多次 flush 时覆盖）
// =============================================================================

#include "dlog/dlog.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <vector>

namespace dlog {

namespace {

// ---------------------------------------------------------------------------
// 格式化当前本地时间为 "20260412_102536"
// ---------------------------------------------------------------------------
std::string timestampNow() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    ::localtime_r(&t, &tm);
    char buf[20];
    std::snprintf(buf, sizeof(buf), "%04d%02d%02d_%02d%02d%02d",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec);
    return buf;
}

// ---------------------------------------------------------------------------
// Logger 单例
// ---------------------------------------------------------------------------
class Logger {
public:
    static Logger &instance() {
        static Logger inst;
        return inst;
    }

    // 配置并启动后台 flush 线程
    void init(const std::string &output_dir, int flush_interval_sec, bool console_echo) {
        std::lock_guard<std::mutex> lk(mu_);
        if (running_.load())
            return; // 已初始化，幂等

        output_dir_          = output_dir;
        flush_interval_sec_  = flush_interval_sec < 1 ? 1 : flush_interval_sec;
        console_echo_        = console_echo;

        // 创建输出目录（如果不存在）
        ::mkdir(output_dir_.c_str(), 0755);

        running_.store(true);
        flush_thread_ = std::thread([this] { flushLoop(); });
    }

    // 写一条日志，格式化后：
    //   立即 → stderr（如果 console_echo_ = true）
    //   缓冲 → pending_（等待后台 flush）
    void log(Level level, const std::string &prefix, const std::string &msg) {
        const std::string entry = format(level, prefix, msg);

        if (console_echo_)
            std::fputs((entry + "\n").c_str(), stderr);

        if (running_.load()) {
            std::lock_guard<std::mutex> lk(mu_);
            pending_.push_back(entry);
        }
    }

    // 冲刷剩余缓冲，停止后台线程
    void shutdown() {
        if (!running_.exchange(false))
            return;
        cv_.notify_all();
        if (flush_thread_.joinable())
            flush_thread_.join();
        // 最后一次 flush
        std::vector<std::string> tail;
        {
            std::lock_guard<std::mutex> lk(mu_);
            tail.swap(pending_);
        }
        if (!tail.empty())
            doFlush(tail);
    }

private:
    Logger() = default;
    ~Logger() { shutdown(); }

    // ---------------------------------------------------------------------------
    // 格式化一行日志
    // 输出：[20260412_102536][I][MOD:hal][lidar:57] message
    // ---------------------------------------------------------------------------
    static std::string format(Level level, const std::string &prefix,
                               const std::string &msg) {
        char lvl = 'D';
        switch (level) {
        case Level::kInfo:  lvl = 'I'; break;
        case Level::kWarn:  lvl = 'W'; break;
        case Level::kError: lvl = 'E'; break;
        default: break;
        }
        return "[" + timestampNow() + "][" + lvl + "]" + prefix + " " + msg;
    }

    // ---------------------------------------------------------------------------
    // 后台循环：每 flush_interval_sec_ 秒唤醒一次，把 pending_ 写入 .log.gz
    // ---------------------------------------------------------------------------
    void flushLoop() {
        while (running_.load()) {
            std::unique_lock<std::mutex> lk(mu_);
            cv_.wait_for(lk, std::chrono::seconds(flush_interval_sec_),
                         [this] { return !running_.load(); });

            std::vector<std::string> batch;
            batch.swap(pending_); // 拿走所有待写条目，立即释放锁
            lk.unlock();

            if (!batch.empty())
                doFlush(batch);
        }
    }

    // ---------------------------------------------------------------------------
    // 把一批日志行写入一个新的 .log.gz 文件
    //
    // 文件名：{output_dir}/dlog_{timestamp}_{seq}.log.gz
    // 写入方式：popen("gzip -c > filepath", "w")
    //   → gzip 从 stdin 读取明文，压缩后写入文件
    //   → 生成标准 gzip 格式，可直接用 zcat 查看
    // ---------------------------------------------------------------------------
    void doFlush(const std::vector<std::string> &lines) {
        const std::string ts  = timestampNow();
        const std::string path = output_dir_ + "/dlog_" + ts + "_" +
                                  std::to_string(flush_seq_++) + ".log.gz";

        // "gzip -c > path" 把 stdin 压缩后写入 path
        const std::string cmd = "gzip -c > " + path;
        FILE *gz = ::popen(cmd.c_str(), "w");
        if (!gz) {
            std::fputs(("[dlog] cannot open gzip pipe: " + path + "\n").c_str(), stderr);
            return;
        }

        for (const auto &line : lines) {
            std::fwrite(line.c_str(), 1, line.size(), gz);
            std::fputc('\n', gz);
        }

        ::pclose(gz);

        // 在 stderr 提示文件位置（仅 console_echo_ 时显示）
        if (console_echo_) {
            std::fputs(("[dlog] flushed " + std::to_string(lines.size()) +
                        " entries → " + path + "\n").c_str(), stderr);
        }
    }

    std::mutex              mu_;
    std::condition_variable cv_;
    std::vector<std::string> pending_;
    std::string             output_dir_{"."};
    int                     flush_interval_sec_{10};
    bool                    console_echo_{true};
    std::atomic<bool>       running_{false};
    std::thread             flush_thread_;
    std::uint32_t           flush_seq_{0};
};

} // namespace

// ---------------------------------------------------------------------------
// 公共 API 实现
// ---------------------------------------------------------------------------

void init(const std::string &output_dir, int flush_interval_sec, bool console_echo) {
    Logger::instance().init(output_dir, flush_interval_sec, console_echo);
}

void shutdown() {
    Logger::instance().shutdown();
}

void log(Level level, const std::string &prefix, const std::string &msg) {
    Logger::instance().log(level, prefix, msg);
}

} // namespace dlog
