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

static void sigHandler(int) {
    g_running = false;
}

int main() {
    dlog::init("/tmp/dcms_logs", 5, /*console_echo=*/true);

    std::signal(SIGINT,  sigHandler);
    std::signal(SIGTERM, sigHandler);

    DAEMON_LOGI("starting on " + std::string(dcms::kDaemonSocketPath));

    dcms::Daemon daemon;
    if (!daemon.init()) {
        DAEMON_LOGE("init failed");
        dlog::shutdown();
        return 1;
    }

    daemon.start();

    while (g_running.load(std::memory_order_relaxed))
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

    DAEMON_LOGI("shutting down");
    daemon.stop();

    dlog::shutdown();
    return 0;
}
