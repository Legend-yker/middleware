// =============================================================================
// pubsub_demo — DCMS 发布订阅 + dlog 日志集成示例
//
// 演示内容：
//   1. dlog 初始化 → 后台每 5s 刷写 .log.gz
//   2. 用 dcms_msg_subscribe 注册两个订阅者（subscriber_A / subscriber_B）
//   3. 发布者线程每 200ms 发布一条 CounterMsg，共 20 条
//   4. 两个订阅者回调各自用 DLOG_INFO 写日志
//   5. 主线程等待发布结束 → 调用 dlog::shutdown() 强制冲刷最后一批日志
//   6. 打印 demo_logs/ 目录下生成的 .log.gz 文件列表
//
// 运行后可用以下命令查看日志：
//   zcat demo_logs/*.log.gz | sort
// =============================================================================

#include "api/dcms_api.h"
#include "api/dcms.h"
#include "dlog/dlog.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iostream>
#include <string>
#include <thread>

// ---------------------------------------------------------------------------
// 消息体定义
// ---------------------------------------------------------------------------
struct CounterMsg {
    std::uint32_t counter;    // 递增计数器
    std::uint64_t sent_us;    // 发送时刻（微秒，steady_clock）
    float         payload;    // 演示用浮点数据
};

// CounterMsg: uint32(4) + pad(4) + uint64(8) + float(4) + pad(4) = 24 bytes
static_assert(sizeof(CounterMsg) == 24, "CounterMsg layout check");

// ---------------------------------------------------------------------------
// topic id：用字符串哈希保证与 topic name 一致
// ---------------------------------------------------------------------------
static constexpr dcms_topic_id kDemoTopic =
    static_cast<dcms_topic_id>(0xDEADBEEFu);  // 演示用固定 ID

// ---------------------------------------------------------------------------
// 全局统计
// ---------------------------------------------------------------------------
namespace {
std::atomic<int>  g_recv_a{0};
std::atomic<int>  g_recv_b{0};
std::atomic<bool> g_running{true};

void onSignal(int) { g_running = false; }

std::uint64_t nowUs() {
    using namespace std::chrono;
    return static_cast<std::uint64_t>(
        duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count());
}

// ---------------------------------------------------------------------------
// 订阅者回调 A — 关注每条消息的往返延迟
// ---------------------------------------------------------------------------
void subscriberA(dcms_topic_id /*topic*/, const void *data,
                 std::uint32_t len, void * /*user*/)
{
    if (len < sizeof(CounterMsg))
        return;

    CounterMsg msg{};
    std::memcpy(&msg, data, sizeof(msg));

    const std::uint64_t latency_us = nowUs() - msg.sent_us;

    DLOG_INFO("[MOD:demo][sub_A]",
              "recv counter=" + std::to_string(msg.counter) +
              " latency_us=" + std::to_string(latency_us) +
              " payload=" + std::to_string(msg.payload));

    g_recv_a.fetch_add(1, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// 订阅者回调 B — 只关注奇数帧
// ---------------------------------------------------------------------------
void subscriberB(dcms_topic_id /*topic*/, const void *data,
                 std::uint32_t len, void * /*user*/)
{
    if (len < sizeof(CounterMsg))
        return;

    CounterMsg msg{};
    std::memcpy(&msg, data, sizeof(msg));

    if (msg.counter % 2 == 0)
        return;  // 只处理奇数帧

    DLOG_INFO("[MOD:demo][sub_B]",
              "odd counter=" + std::to_string(msg.counter) +
              " payload=" + std::to_string(msg.payload));

    g_recv_b.fetch_add(1, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// 发布者线程
// ---------------------------------------------------------------------------
void publisherThread(int total_msgs, int interval_ms)
{
    DLOG_INFO("[MOD:demo][publisher]",
              "start total=" + std::to_string(total_msgs) +
              " interval_ms=" + std::to_string(interval_ms));

    for (int i = 0; i < total_msgs && g_running.load(std::memory_order_relaxed); ++i) {
        CounterMsg msg{};
        msg.counter  = static_cast<std::uint32_t>(i + 1);
        msg.sent_us  = nowUs();
        msg.payload  = static_cast<float>(i) * 3.14f;

        const int rc = dcms_msg_publish(kDemoTopic, &msg, sizeof(msg));
        if (rc == 0) {
            DLOG_INFO("[MOD:demo][publisher]",
                      "published counter=" + std::to_string(msg.counter));
        } else {
            DLOG_WARN("[MOD:demo][publisher]",
                      "publish failed counter=" + std::to_string(msg.counter) +
                      " rc=" + std::to_string(rc));
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
    }

    DLOG_INFO("[MOD:demo][publisher]", "done");
    g_running = false;
}

} // namespace

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main()
{
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    // ① 初始化 dlog：日志写入 ./demo_logs，每 5s 刷写一次 .log.gz
    dlog::init("./demo_logs", 5, /*console_echo=*/true);
    DLOG_INFO("[MOD:demo]", "pubsub_demo started");

    // ② 初始化 DCMS（socketpair 模式，无需独立 broker 进程）
    const int dcms_rc = dcms::init();
    if (dcms_rc != 0) {
        DLOG_ERROR("[MOD:demo]", "dcms init failed rc=" + std::to_string(dcms_rc));
        dlog::shutdown();
        return 1;
    }
    DLOG_INFO("[MOD:demo]", "dcms initialized");

    // ③ 注册两个订阅者
    dcms_msg_subscribe(kDemoTopic, subscriberA, nullptr);
    dcms_msg_subscribe(kDemoTopic, subscriberB, nullptr);
    DLOG_INFO("[MOD:demo]", "subscribers registered topic=0x" +
              []{ char buf[16]; std::snprintf(buf, sizeof(buf), "%08X", kDemoTopic); return std::string(buf); }());

    // ④ 启动发布者线程：发 20 条，间隔 200ms（总计约 4s）
    constexpr int kTotalMsgs   = 20;
    constexpr int kIntervalMs  = 200;
    std::thread pub_thread(publisherThread, kTotalMsgs, kIntervalMs);

    // ⑤ 等待发布完成
    pub_thread.join();

    // ⑥ 打印统计
    DLOG_INFO("[MOD:demo]",
              "finished recv_a=" + std::to_string(g_recv_a.load()) +
              " recv_b=" + std::to_string(g_recv_b.load()) +
              " (expected recv_a=" + std::to_string(kTotalMsgs) +
              " recv_b=" + std::to_string(kTotalMsgs / 2) + ")");

    // ⑦ 清理
    dcms_msg_unsubscribe(kDemoTopic, subscriberB, nullptr);
    dcms_msg_unsubscribe(kDemoTopic, subscriberA, nullptr);
    dcms::shutdown();

    // ⑧ 强制冲刷剩余日志 → .log.gz 文件
    dlog::shutdown();

    // ⑨ 提示用户查看生成的日志文件
    std::cout << "\n[pubsub_demo] Log files in ./demo_logs/:\n";
    { int rc = std::system("ls -lh ./demo_logs/*.log.gz 2>/dev/null || echo '  (no files yet)'"); (void)rc; }
    std::cout << "[pubsub_demo] Read with: zcat ./demo_logs/*.log.gz | sort\n";

    return 0;
}
