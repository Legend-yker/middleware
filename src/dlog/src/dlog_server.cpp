// =============================================================================
// dlog_server — 多进程日志汇聚服务
//
// 启动后监听 /tmp/dlog_server.sock，接受任意数量的客户端连接。
// 所有客户端（hal 进程、dcms 进程、测试进程……）调用 dlog::init() 时会
// 自动检测并连接到此 server，之后 log() 的每条记录都发到这里。
//
// dlog_server 把所有进程的日志汇聚进同一个缓冲区，
// 每 flush_interval_sec 秒写入同一个 .log 文本文件（O_APPEND）；
// 正常退出时压缩为 .log.gz（gzip -f），然后删除 .log。
//
// 协议：客户端发 [uint32_t len_LE][log_line UTF-8，不含 '\n']
//
// 用法：
//   ./dlog_server [output_dir] [flush_interval_sec]
//   默认：output_dir=/tmp/dcms_logs  flush_interval_sec=10
//
//   Ctrl+C 优雅退出：flush 剩余日志 → gzip → 删除 .log。
// =============================================================================

#include "dlog/dlog.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <mutex>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>
#include <vector>

// ── 全局状态 ──────────────────────────────────────────────────────────────────

static std::atomic<bool>        g_running{true};
static std::mutex               g_mu;
static std::condition_variable  g_cv;
static std::vector<std::string> g_pending;
static std::string              g_output_dir;
static int                      g_flush_interval_sec = 10;

// 整个 server 运行期间写同一个 .log 纯文本文件（O_APPEND），
// 退出时由 gzip -f 压缩为 .log.gz 并删除 .log。
static std::string g_log_path;      // .log  (运行期间写入)
static std::string g_gz_path;       // .log.gz (退出后生成)

static void sigHandler(int) { g_running = false; g_cv.notify_all(); }

// ── 工具函数 ──────────────────────────────────────────────────────────────────

static std::string timestampNow() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    ::localtime_r(&t, &tm);
    char buf[20];
    std::snprintf(buf, sizeof(buf), "%04d%02d%02d_%02d%02d%02d",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec);
    return buf;
}

// 把一批日志行以 O_APPEND 方式追加到 g_log_path
static void doFlush(std::vector<std::string> lines) {
    if (lines.empty()) return;

    int fd = ::open(g_log_path.c_str(),
                    O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) {
        std::fprintf(stderr, "[dlog_server] cannot open log file: %s\n",
                     g_log_path.c_str());
        return;
    }
    for (const auto &line : lines) {
        ::write(fd, line.c_str(), line.size());
        ::write(fd, "\n", 1);
    }
    ::close(fd);

    std::fprintf(stderr, "[dlog_server] flushed %zu entries → %s\n",
                 lines.size(), g_log_path.c_str());
}

// 关闭时压缩 .log → .log.gz，然后删除 .log
static void compressLog() {
    const std::string cmd = "gzip -f " + g_log_path;
    int rc = std::system(cmd.c_str());
    if (rc == 0) {
        // gzip -f 已把 .log 替换为 .log.gz
        std::fprintf(stderr, "[dlog_server] compressed → %s\n",
                     g_gz_path.c_str());
    } else {
        std::fprintf(stderr,
                     "[dlog_server] gzip failed (rc=%d), plain log at %s\n",
                     rc, g_log_path.c_str());
    }
}

// ── 每个客户端连接在独立线程里读日志条目 ─────────────────────────────────────

static void clientThread(int client_fd) {
    while (g_running.load(std::memory_order_relaxed)) {
        // 读长度前缀
        std::uint32_t len = 0;
        std::size_t   got = 0;
        while (got < sizeof(len)) {
            ssize_t r = ::recv(client_fd,
                               reinterpret_cast<char *>(&len) + got,
                               sizeof(len) - got, 0);
            if (r <= 0) goto done;
            got += static_cast<std::size_t>(r);
        }
        if (len == 0 || len > 4096) continue; // 异常长度，跳过

        // 读日志行
        {
            std::string entry(len, '\0');
            got = 0;
            while (got < len) {
                ssize_t r = ::recv(client_fd, &entry[got], len - got, 0);
                if (r <= 0) goto done;
                got += static_cast<std::size_t>(r);
            }
            std::lock_guard<std::mutex> lk(g_mu);
            g_pending.push_back(std::move(entry));
        }
    }
done:
    ::close(client_fd);
}

// ── 定时 flush 线程 ───────────────────────────────────────────────────────────

static void flushLoop() {
    while (g_running.load()) {
        std::unique_lock<std::mutex> lk(g_mu);
        g_cv.wait_for(lk, std::chrono::seconds(g_flush_interval_sec),
                      [] { return !g_running.load(); });
        std::vector<std::string> batch;
        batch.swap(g_pending);
        lk.unlock();
        doFlush(std::move(batch));
    }
}

// ── main ──────────────────────────────────────────────────────────────────────

int main(int argc, char *argv[]) {
    g_output_dir         = (argc > 1) ? argv[1] : "/tmp/dcms_logs";
    g_flush_interval_sec = (argc > 2) ? std::atoi(argv[2]) : 10;
    if (g_flush_interval_sec < 1) g_flush_interval_sec = 1;

    ::mkdir(g_output_dir.c_str(), 0755);

    // 信号处理必须在 popen/system 之前注册，避免子进程继承
    std::signal(SIGINT,  sigHandler);
    std::signal(SIGTERM, sigHandler);

    // ── 日志文件路径（.log 文本，退出时压缩）───────────────────────────────
    const std::string ts = timestampNow();
    g_log_path = g_output_dir + "/dlog_" + ts + ".log";
    g_gz_path  = g_log_path + ".gz";

    // ── 创建监听 socket ──────────────────────────────────────────────────────
    ::unlink(dlog::kServerSocketPath);
    int listen_fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (listen_fd < 0) { std::perror("socket"); return 1; }

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, dlog::kServerSocketPath,
                 sizeof(addr.sun_path) - 1);

    if (::bind(listen_fd, reinterpret_cast<sockaddr *>(&addr),
               sizeof(addr)) < 0) {
        std::perror("bind"); return 1;
    }
    ::chmod(dlog::kServerSocketPath, 0666);
    ::listen(listen_fd, 32);

    std::fprintf(stderr,
                 "[dlog_server] listening on %s  output=%s  flush=%ds\n"
                 "[dlog_server] log file → %s  (compressed to .gz on exit)\n",
                 dlog::kServerSocketPath, g_output_dir.c_str(),
                 g_flush_interval_sec, g_log_path.c_str());

    // ── 启动 flush 线程 ──────────────────────────────────────────────────────
    std::thread flusher(flushLoop);

    // ── accept 循环（主线程）────────────────────────────────────────────────
    while (g_running.load()) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(listen_fd, &rfds);
        timeval tv{0, 200000}; // 200ms
        if (::select(listen_fd + 1, &rfds, nullptr, nullptr, &tv) <= 0)
            continue;

        int client_fd = ::accept4(listen_fd, nullptr, nullptr, SOCK_CLOEXEC);
        if (client_fd < 0) continue;

        std::fprintf(stderr, "[dlog_server] client connected fd=%d\n",
                     client_fd);
        std::thread(clientThread, client_fd).detach();
    }

    // ── 优雅关闭 ─────────────────────────────────────────────────────────────
    ::close(listen_fd);
    ::unlink(dlog::kServerSocketPath);

    g_cv.notify_all();
    flusher.join();

    // 最终 flush
    {
        std::vector<std::string> tail;
        {
            std::lock_guard<std::mutex> lk(g_mu);
            tail.swap(g_pending);
        }
        doFlush(std::move(tail));
    }

    // 压缩 .log → .log.gz（gzip -f 只在正常关闭路径执行，不受 SIGINT 影响）
    compressLog();

    std::fprintf(stderr, "[dlog_server] shutdown complete, log → %s\n",
                 g_gz_path.c_str());
    return 0;
}
