// =============================================================================
// pubsub_demo — DCMS 发布订阅 + dlog 日志集成示例
//
// 演示内容：
//   1. dlog 初始化 → 后台每 5s 刷写 .log.gz
//   2. 用 NodeHandle::subscribe 注册两个订阅者（subscriber_A / subscriber_B）
//   3. NodeHandle::advertise 发布者每 200ms 发布一条 CounterMsg，共 20 条
//   4. 两个订阅者回调各自用 DLOG_INFO 写日志
//   5. 主线程等待发布结束 → dlog::shutdown() 强制冲刷最后一批日志
//
// 运行前需要先启动 dcms_daemon 和 dlog_server（可选）：
//   ./dlog_server &
//   ./dcms_daemon &
//   ./pubsub_demo
//   zcat /tmp/dcms_logs/*.log.gz | sort
// =============================================================================

#include "api/dcms.h"
#include "api/node_handle.h"
#include "dlog/dlog.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <iostream>
#include <string>
#include <thread>

// ---------------------------------------------------------------------------
// 消息体定义
// ---------------------------------------------------------------------------
struct CounterMsg {
    std::uint32_t counter;
    std::uint64_t sent_us;
    float         payload;
};
static_assert(sizeof(CounterMsg) == 24, "CounterMsg layout check");

static constexpr const char *kDemoTopic = "demo_counter";

namespace {

std::atomic<int>  g_recv_a{0};
std::atomic<int>  g_recv_b{0};
std::atomic<bool> g_running{true};

void onSignal(int) { g_running = false; }

std::uint64_t nowUs() {
    using namespace std::chrono;
    return static_cast<std::uint64_t>(
        duration_cast<microseconds>(
            steady_clock::now().time_since_epoch()).count());
}

} // namespace

int main() {
    std::signal(SIGINT,  onSignal);
    std::signal(SIGTERM, onSignal);

    dlog::init("/tmp/dcms_logs", 5, /*console_echo=*/true);
    DLOG_INFO("[MOD:demo]", "pubsub_demo started");

    const int dcms_rc = dcms::init("pubsub_demo");
    if (dcms_rc != dcms::kDcmsOk) {
        DLOG_ERROR("[MOD:demo]", "dcms init failed rc=" + std::to_string(dcms_rc));
        dlog::shutdown();
        return 1;
    }
    DLOG_INFO("[MOD:demo]", "dcms initialized");

    auto &nh = dcms::defaultNode();

    // ── 注册两个订阅者 ────────────────────────────────────────────────────────
    auto sub_a = nh.subscribe<CounterMsg>(
        kDemoTopic, [](const CounterMsg &msg) {
            const std::uint64_t latency = nowUs() - msg.sent_us;
            DLOG_INFO("[MOD:demo][sub_A]",
                      "recv counter=" + std::to_string(msg.counter) +
                      " latency_us=" + std::to_string(latency));
            g_recv_a.fetch_add(1, std::memory_order_relaxed);
        });

    auto sub_b = nh.subscribe<CounterMsg>(
        kDemoTopic, [](const CounterMsg &msg) {
            if (msg.counter % 2 == 0) return;
            DLOG_INFO("[MOD:demo][sub_B]",
                      "odd counter=" + std::to_string(msg.counter));
            g_recv_b.fetch_add(1, std::memory_order_relaxed);
        });

    DLOG_INFO("[MOD:demo]", "subscribers registered topic=" + std::string(kDemoTopic));

    // ── 发布者线程 ────────────────────────────────────────────────────────────
    auto pub = nh.advertise<CounterMsg>(kDemoTopic);

    constexpr int kTotalMsgs  = 20;
    constexpr int kIntervalMs = 200;

    std::thread pub_thread([&pub, kIntervalMs] {
        DLOG_INFO("[MOD:demo][publisher]", "start");
        for (int i = 0; i < kTotalMsgs && g_running.load(); ++i) {
            CounterMsg msg{};
            msg.counter = static_cast<std::uint32_t>(i + 1);
            msg.sent_us = nowUs();
            msg.payload = static_cast<float>(i) * 3.14f;

            const int rc = pub.publish(msg);
            if (rc == 0) {
                DLOG_INFO("[MOD:demo][publisher]",
                          "published counter=" + std::to_string(msg.counter));
            } else {
                DLOG_WARN("[MOD:demo][publisher]",
                          "publish failed counter=" + std::to_string(msg.counter) +
                          " rc=" + std::to_string(rc));
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(kIntervalMs));
        }
        DLOG_INFO("[MOD:demo][publisher]", "done");
        g_running = false;
    });

    pub_thread.join();

    DLOG_INFO("[MOD:demo]",
              "finished recv_a=" + std::to_string(g_recv_a.load()) +
              " recv_b=" + std::to_string(g_recv_b.load()));

    dcms::shutdown();
    dlog::shutdown();

    std::cout << "\n[pubsub_demo] done. Read logs with:\n"
              << "  zcat /tmp/dcms_logs/*.log.gz | sort\n";
    return 0;
}
