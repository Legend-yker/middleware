#include "daemon/daemon.h"
#include "daemon/daemon_protocol.h"

#include "dlog/dlog.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <thread>

#define DAEMON_LOGI(msg) DLOG_INFO("[MOD:dcms_daemon]", msg)
#define DAEMON_LOGE(msg) DLOG_ERROR("[MOD:dcms_daemon]", msg)

static std::atomic<bool> g_running{true};

// Ctrl+C 或 kill 信号处理：置 false，让主循环退出
static void sigHandler(int) {
    g_running = false;
}

int main() {
    dlog::init("/tmp/dcms_logs", 5, /*console_echo=*/true);

    std::signal(SIGINT,  sigHandler); // Ctrl+C
    std::signal(SIGTERM, sigHandler); // systemd stop / kill

    DAEMON_LOGI("starting on " + std::string(dcms::kDaemonSocketPath));

    dcms::Daemon daemon;
    if (!daemon.init()) { // 绑定 Unix socket + 创建 epoll
        DAEMON_LOGE("init failed");
        dlog::shutdown();
        return 1;
    }

    daemon.start(); // 启动 acceptLoop 后台线程

    // 主线程只做信号等待，100ms 轮询一次 g_running
    while (g_running.load(std::memory_order_relaxed))
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

    DAEMON_LOGI("shutting down");
    daemon.stop(); // 通知 acceptLoop 退出并 join

    dlog::shutdown();
    return 0;
}
