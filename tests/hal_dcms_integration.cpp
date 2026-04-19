// hal_dcms_integration: 全模块集成测试
//
// 整个测试跑在 **同一个进程** 里：
//   - dcms::Daemon        运行在独立线程（非独立进程）
//   - [MOD:hal]           模拟 HAL 传感器，每 100ms 发布一帧 SensorFrame
//   - [MOD:nav]           导航模块，订阅 SensorFrame，计算里程并打印
//   - [MOD:logger_mod]    日志模块，订阅同一 topic，仅做统计
//   - [MOD:rpc_server]    RPC 服务，供调用方查询当前收到的帧数
//   - [MOD:rpc_client]    RPC 客户端，向上面的 server 查询帧数
//
// 因为全部在同一进程，dlog Logger 单例只有一个：
//   → 所有模块的日志写到同一个 /tmp/dcms_logs/dlog_XXXX.log.gz
//
// 查询示例：
//   zcat /tmp/dcms_logs/dlog_*.log.gz | grep 'MOD:hal'
//   zcat /tmp/dcms_logs/dlog_*.log.gz | grep 'MOD:nav'
//   zcat /tmp/dcms_logs/dlog_*.log.gz | grep 'MOD:dcms_daemon'

#include "daemon/daemon.h"
#include "daemon/daemon_protocol.h"
#include "api/node_handle.h"
#include "api/service_server.h"
#include "api/service_client.h"

#include "dlog/dlog.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

// ── 模块日志宏 ─────────────────────────────────────────────────────────────────

#define HAL_LOG(msg)     DLOG_INFO("[MOD:hal]",        msg)
#define NAV_LOG(msg)     DLOG_INFO("[MOD:nav]",        msg)
#define LOGGER_LOG(msg)  DLOG_INFO("[MOD:logger_mod]", msg)
#define RPC_SRV_LOG(msg) DLOG_INFO("[MOD:rpc_server]", msg)
#define RPC_CLI_LOG(msg) DLOG_INFO("[MOD:rpc_client]", msg)

// ── 消息类型 ───────────────────────────────────────────────────────────────────

struct SensorFrame {
    std::uint32_t frame_id;
    std::uint64_t timestamp_us;
    float         accel[3];   // 模拟 IMU 加速度
    float         gyro[3];    // 模拟 IMU 角速度
    std::uint32_t seq;
};

struct QueryReq  { std::uint32_t dummy; };
struct QueryResp { std::uint32_t frames_received; std::uint32_t frames_published; };

// ── 工具函数 ───────────────────────────────────────────────────────────────────

static std::uint64_t nowUs() {
    using namespace std::chrono;
    return static_cast<std::uint64_t>(
        duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count());
}

// ── main ───────────────────────────────────────────────────────────────────────

int main() {
    // 所有模块共用同一个 dlog 实例（进程内单例）
    dlog::init("/tmp/dcms_logs", 5, /*console_echo=*/true);
    DLOG_INFO("[MOD:integration]", "===== hal_dcms_integration start =====");

    // ── 1. 在线程里启动 Daemon（非独立进程，共享 Logger）─────────────────────────
    dcms::Daemon daemon;
    std::thread daemon_thread([&daemon] { daemon.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(100)); // 等 epoll 就绪
    DLOG_INFO("[MOD:integration]", "daemon thread started");

    // ── 2. 创建各模块的 NodeHandle ────────────────────────────────────────────
    dcms::NodeHandle hal_node, nav_node, logger_node;

    assert(dcms::NodeHandle::create("hal_module",    hal_node)    == dcms::kDcmsOk);
    assert(dcms::NodeHandle::create("nav_module",    nav_node)    == dcms::kDcmsOk);
    assert(dcms::NodeHandle::create("logger_module", logger_node) == dcms::kDcmsOk);
    DLOG_INFO("[MOD:integration]", "all nodes connected to daemon");

    // ── 3. HAL 模块：advertise ─────────────────────────────────────────────────
    auto pub = hal_node.advertise<SensorFrame>("hal/imu", /*ring_slots=*/16);
    assert(pub.valid());
    HAL_LOG("advertised topic hal/imu slot_size=" + std::to_string(sizeof(SensorFrame)));

    // ── 4. NAV 模块：subscribe ─────────────────────────────────────────────────
    std::atomic<std::uint32_t> nav_received{0};
    float nav_accel_sum = 0.f;

    auto nav_sub = nav_node.subscribe<SensorFrame>(
        "hal/imu",
        [&](const SensorFrame &f) {
            nav_accel_sum += f.accel[0];
            std::uint32_t n = nav_received.fetch_add(1, std::memory_order_relaxed) + 1;
            NAV_LOG("recv frame_id=" + std::to_string(f.frame_id) +
                    " seq=" + std::to_string(f.seq) +
                    " accel_x=" + std::to_string(f.accel[0]) +
                    " total=" + std::to_string(n));
        });
    assert(nav_sub.valid());
    NAV_LOG("subscribed to hal/imu");

    // ── 5. LOGGER 模块：subscribe（另一个订阅者，同一 topic）──────────────────
    std::atomic<std::uint32_t> log_received{0};

    auto log_sub = logger_node.subscribe<SensorFrame>(
        "hal/imu",
        [&](const SensorFrame &f) {
            std::uint32_t n = log_received.fetch_add(1, std::memory_order_relaxed) + 1;
            LOGGER_LOG("frame_id=" + std::to_string(f.frame_id) +
                       " ts_us=" + std::to_string(f.timestamp_us) +
                       " total_logged=" + std::to_string(n));
        });
    assert(log_sub.valid());
    LOGGER_LOG("subscribed to hal/imu");

    // 等待 Daemon 把 kNewSubscriberNotify 推送到 publisher
    std::this_thread::sleep_for(std::chrono::milliseconds(80));

    // ── 6. RPC 服务：查询当前收帧数 ──────────────────────────────────────────
    auto rpc_srv = hal_node.advertiseService<QueryReq, QueryResp>(
        "hal/query_stats",
        [&](const QueryReq &, QueryResp &resp) {
            resp.frames_received  = nav_received.load();
            resp.frames_published = 0; // 在 publish 循环里填充
            RPC_SRV_LOG("query_stats: nav_received=" +
                        std::to_string(resp.frames_received));
        });
    assert(rpc_srv.valid());
    RPC_SRV_LOG("service hal/query_stats ready at /tmp/dcms_svc_hal_query_stats.sock");

    // ── 7. HAL 发布循环：模拟 10Hz 传感器，发 20 帧 ──────────────────────────
    constexpr int kTotalFrames = 20;

    for (std::uint32_t i = 0; i < kTotalFrames; ++i) {
        SensorFrame f{};
        f.frame_id     = 1000 + i;
        f.timestamp_us = nowUs();
        f.accel[0]     = static_cast<float>(i) * 0.1f;
        f.accel[1]     = static_cast<float>(i) * 0.05f;
        f.accel[2]     = 9.8f;
        f.gyro[0]      = static_cast<float>(i) * 0.01f;
        f.gyro[1]      = static_cast<float>(i) * 0.02f;
        f.gyro[2]      = 0.f;
        f.seq          = i;

        int rc = pub.publish(f);
        assert(rc == dcms::kDcmsOk);
        HAL_LOG("publish frame_id=" + std::to_string(f.frame_id) +
                " seq=" + std::to_string(f.seq) +
                " accel_x=" + std::to_string(f.accel[0]));

        std::this_thread::sleep_for(std::chrono::milliseconds(50)); // 20Hz
    }
    HAL_LOG("publish done total=" + std::to_string(kTotalFrames));

    // 等待最后一批消息被订阅者消费
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // ── 8. RPC 查询：client 向 server 询问统计 ────────────────────────────────
    dcms::ServiceClient<QueryReq, QueryResp> rpc_cli("hal/query_stats");
    QueryReq  req{0};
    QueryResp resp{};
    int rc = rpc_cli.call(req, resp);
    assert(rc == dcms::kDcmsOk);
    RPC_CLI_LOG("query result: nav_received=" + std::to_string(resp.frames_received));

    // ── 9. 断言验证 ───────────────────────────────────────────────────────────
    std::uint32_t nav_n = nav_received.load();
    std::uint32_t log_n = log_received.load();

    DLOG_INFO("[MOD:integration]",
              "nav_received=" + std::to_string(nav_n) +
              " log_received=" + std::to_string(log_n) +
              " expected=" + std::to_string(kTotalFrames));

    assert(nav_n == kTotalFrames);
    assert(log_n == kTotalFrames);
    assert(resp.frames_received == kTotalFrames);

    DLOG_INFO("[MOD:integration]", "===== hal_dcms_integration: all assertions PASS =====");
    std::cout << "hal_dcms_integration: ok\n";

    // ── 10. 优雅关闭 ──────────────────────────────────────────────────────────
    daemon.stop();
    daemon_thread.join();
    DLOG_INFO("[MOD:integration]", "daemon stopped");

    // shutdown 强制冲刷所有缓冲（所有模块的日志都在这一次写入同一个文件）
    dlog::shutdown();
    return EXIT_SUCCESS;
}
