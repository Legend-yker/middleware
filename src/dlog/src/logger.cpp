// =============================================================================
// logger.cpp — dlog 日志系统实现
//
// 两种运行模式（init() 时自动选择）：
//
//   远程模式（dlog_server 正在运行时自动启用）
//   ─────────────────────────────────────────
//   log() → 格式化 → writev([uint32_t len][text]) 发给 dlog_server
//          → 同时打印 stderr（若 console_echo=true）
//   dlog_server 汇聚所有进程的条目，写同一个 .log.gz
//
//   本地模式（dlog_server 未运行时回退）
//   ─────────────────────────────────────
//   log() → 格式化 → 立即打印 stderr → push pending_
//   后台 flush 线程每 N 秒把 pending_ 写入本进程私有 .log.gz
//
// 协议（远程模式）：[uint32_t len_LE][log_line UTF-8，不含 '\n']
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
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace dlog {

namespace {

// ---------------------------------------------------------------------------
// 格式化时间戳 "20260412_102536"
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
// 格式化一行日志
// ---------------------------------------------------------------------------
static std::string formatEntry(Level level, const std::string &prefix,
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
// Logger 单例
// ---------------------------------------------------------------------------
class Logger {
public:
    static Logger &instance() {
        static Logger inst;
        return inst;
    }

    void init(const std::string &output_dir, int flush_interval_sec,
              bool console_echo) {
        std::lock_guard<std::mutex> lk(mu_);
        if (initialized_) return;

        output_dir_         = output_dir;
        flush_interval_sec_ = flush_interval_sec < 1 ? 1 : flush_interval_sec;
        console_echo_       = console_echo;
        initialized_        = true;

        // ── 先尝试连接 dlog_server ───────────────────────────────────────
        int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd >= 0) {
            sockaddr_un addr{};
            addr.sun_family = AF_UNIX;
            std::strncpy(addr.sun_path, kServerSocketPath,
                         sizeof(addr.sun_path) - 1);
            if (::connect(fd, reinterpret_cast<sockaddr *>(&addr),
                          sizeof(addr)) == 0) {
                server_fd_ = fd;
                // 远程模式：不需要本地 flush 线程
                if (console_echo_)
                    std::fputs("[dlog] connected to dlog_server (remote mode)\n",
                               stderr);
                return;
            }
            ::close(fd);
        }

        // ── 本地模式：启动 flush 线程 ────────────────────────────────────
        ::mkdir(output_dir_.c_str(), 0755);
        running_.store(true);
        flush_thread_ = std::thread([this] { flushLoop(); });
    }

    void log(Level level, const std::string &prefix, const std::string &msg) {
        const std::string entry = formatEntry(level, prefix, msg);

        if (console_echo_)
            std::fputs((entry + "\n").c_str(), stderr);

        if (server_fd_ >= 0) {
            // 远程模式：发给 dlog_server
            std::uint32_t len = static_cast<std::uint32_t>(entry.size());
            iovec iov[2];
            iov[0].iov_base = &len;
            iov[0].iov_len  = sizeof(len);
            iov[1].iov_base = const_cast<char *>(entry.data());
            iov[1].iov_len  = entry.size();

            std::lock_guard<std::mutex> lk(send_mu_);
            ::writev(server_fd_, iov, 2);
        } else if (running_.load()) {
            // 本地模式：写入缓冲区
            std::lock_guard<std::mutex> lk(mu_);
            pending_.push_back(entry);
        }
    }

    void shutdown() {
        if (server_fd_ >= 0) {
            ::close(server_fd_);
            server_fd_ = -1;
            initialized_ = false;
            return;
        }
        if (!running_.exchange(false)) return;
        cv_.notify_all();
        if (flush_thread_.joinable())
            flush_thread_.join();
        std::vector<std::string> tail;
        {
            std::lock_guard<std::mutex> lk(mu_);
            tail.swap(pending_);
        }
        if (!tail.empty()) doFlush(tail);
        initialized_ = false;
    }

private:
    Logger() = default;
    ~Logger() { shutdown(); }

    void flushLoop() {
        while (running_.load()) {
            std::unique_lock<std::mutex> lk(mu_);
            cv_.wait_for(lk, std::chrono::seconds(flush_interval_sec_),
                         [this] { return !running_.load(); });
            std::vector<std::string> batch;
            batch.swap(pending_);
            lk.unlock();
            if (!batch.empty()) doFlush(batch);
        }
    }

    void doFlush(const std::vector<std::string> &lines) {
        const std::string ts   = timestampNow();
        const std::string path = output_dir_ + "/dlog_" + ts + "_" +
                                 std::to_string(flush_seq_++) + ".log.gz";
        const std::string cmd  = "gzip -c > " + path;
        FILE *gz = ::popen(cmd.c_str(), "w");
        if (!gz) {
            std::fputs(("[dlog] cannot open gzip pipe: " + path + "\n").c_str(),
                       stderr);
            return;
        }
        for (const auto &line : lines) {
            std::fwrite(line.c_str(), 1, line.size(), gz);
            std::fputc('\n', gz);
        }
        ::pclose(gz);
        if (console_echo_)
            std::fputs(("[dlog] flushed " + std::to_string(lines.size()) +
                        " entries → " + path + "\n").c_str(), stderr);
    }

    // 本地模式共用
    std::mutex              mu_;
    std::condition_variable cv_;
    std::vector<std::string> pending_;
    std::string             output_dir_{"."};
    int                     flush_interval_sec_{10};
    bool                    console_echo_{true};
    std::atomic<bool>       running_{false};
    std::thread             flush_thread_;
    std::uint32_t           flush_seq_{0};
    bool                    initialized_{false};

    // 远程模式
    int        server_fd_{-1};
    std::mutex send_mu_;  // 保护 writev（多线程同时调用 log() 时）
};

} // namespace

void init(const std::string &output_dir, int flush_interval_sec,
          bool console_echo) {
    Logger::instance().init(output_dir, flush_interval_sec, console_echo);
}

void shutdown() {
    Logger::instance().shutdown();
}

void log(Level level, const std::string &prefix, const std::string &msg) {
    Logger::instance().log(level, prefix, msg);
}

} // namespace dlog
