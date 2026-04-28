// =============================================================================
// lidar_frame_manager_test.cpp — LidarFrameManager 单元测试
//
// 不依赖硬件，直接构造点云数据喂给 FrameManager，验证：
//   1. 包数触发帧完成
//   2. 方位角回绕触发帧边界
//   3. 帧超时强制发布
//   4. 时间戳法丢包检测（gap_curr > ratio × pkt_gap）
//   5. 序列号法丢包检测（seq 跳变）
//   6. 无效点过滤（距离越界）
//   7. 角度覆盖率计算
//   8. 帧率计算（不为零）
// =============================================================================
#include "sensor/lidar/common/lidar_frame_manager.h"
#include "sensor/lidar/common/lidar_frame.h"

#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

// ── 辅助：打印测试结果 ────────────────────────────────────────────────────────
static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond, name)                                                \
    do {                                                                 \
        if (cond) {                                                      \
            ++g_pass;                                                    \
            std::printf("  [PASS] %s\n", name);                         \
        } else {                                                         \
            ++g_fail;                                                    \
            std::printf("  [FAIL] %s  (line %d)\n", name, __LINE__);    \
        }                                                                \
    } while (0)

// ── 辅助：构造一个合法的点（距离在有效范围内）────────────────────────────────
static hal::LidarPoint makePoint(float x, float y, float z) {
    hal::LidarPoint p{};
    p.x = x; p.y = y; p.z = z;
    p.intensity = 100; p.ring = 0;
    return p;
}

// ── 辅助：当前微秒时间 ────────────────────────────────────────────────────────
static std::uint64_t nowUs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

// ── 辅助：生成一包 PktInfo ────────────────────────────────────────────────────
static hal::LidarFrameManager::PktInfo makePkt(
    std::uint64_t device_us, float angle_start, float angle_end,
    std::uint32_t seq = 0, bool has_seq = false) {
    hal::LidarFrameManager::PktInfo info{};
    info.device_time_us = device_us;
    info.recv_time_us   = nowUs();
    info.angle_start    = angle_start;
    info.angle_end      = angle_end;
    info.seq            = seq;
    info.has_seq        = has_seq;
    return info;
}

// ── 构造默认配置（10Hz，60包/帧，600µs包间隔）────────────────────────────────
static hal::LidarFrameManager::Config makeConfig() {
    hal::LidarFrameManager::Config cfg{};
    cfg.pkts_per_frame    = 60;
    cfg.frame_timeout_us  = 150000;   // 150ms
    cfg.max_points        = hal::LidarFrame::kMaxPoints;
    cfg.pkt_gap_us        = 1666;     // ~600包/s
    cfg.drop_ratio        = 1.8;
    cfg.has_seq           = true;
    cfg.frame_start_angle = 0.0f;
    cfg.angle_tol         = 2.0f;
    cfg.min_dist_m        = 0.1f;
    cfg.max_dist_m        = 200.0f;
    return cfg;
}

// =============================================================================
// 测试1：包数达到 pkts_per_frame 时触发帧完成
// =============================================================================
static void test_count_trigger() {
    std::printf("\n[Test1] 包数触发帧完成\n");
    auto cfg = makeConfig();
    cfg.frame_start_angle = 999.0f;  // 不用方位角触发
    cfg.angle_tol         = 0.0f;
    hal::LidarFrameManager mgr(cfg);

    hal::LidarPoint pts[4];
    for (int i = 0; i < 4; ++i) pts[i] = makePoint(1.0f, 1.0f, 1.0f);

    std::uint64_t t = 1000000;
    bool ready = false;
    for (std::uint32_t i = 0; i < cfg.pkts_per_frame; ++i) {
        auto info = makePkt(t + i * cfg.pkt_gap_us, 10.0f, 12.0f);
        ready = mgr.feed(pts, 4, info);
        if (ready) break;
    }

    CHECK(ready, "pkts_per_frame 包后帧就绪");
    if (ready) {
        auto frame = mgr.take();
        CHECK(frame.status == hal::LidarFrame::Status::kOk, "帧状态 kOk");
        CHECK(frame.pkt_got == cfg.pkts_per_frame, "pkt_got 正确");
        CHECK(frame.num_points == cfg.pkts_per_frame * 4, "点数正确");
    }
    CHECK(mgr.stats().frame_ok == 1, "frame_ok 计数=1");
}

// =============================================================================
// 测试2：方位角回绕触发帧边界（359°→0°）
// =============================================================================
static void test_azimuth_boundary() {
    std::printf("\n[Test2] 方位角回绕触发帧边界\n");
    auto cfg = makeConfig();
    cfg.pkts_per_frame = 1000;       // 包数足够大，不走包数触发
    hal::LidarFrameManager mgr(cfg);

    hal::LidarPoint pts[2];
    pts[0] = makePoint(5.0f, 0.0f, 0.0f);
    pts[1] = makePoint(5.0f, 0.0f, 0.1f);

    std::uint64_t t = 1000000;
    // 喂 12 包：角度从 0° 到 330°，每包步进 30°
    // 最后一包 end = 330°+30°=360°，last_angle_ = 360° 会被 markAngle 用到
    // 但我们只关心 last_angle_（angle_end），让它 >= 270° 以触发边界
    for (int i = 0; i < 12; ++i) {
        float a = static_cast<float>(i) * 30.0f;
        // 每包覆盖 30°，angle_end = a + 29.9°
        mgr.feed(pts, 2, makePkt(t + i * 1666, a, a + 29.9f));
    }
    // 此时 last_angle_ ≈ 359.9°（>= 270°），喂一包 angle_start=1° (<90°) → 触发边界
    auto info = makePkt(t + 12 * 1666, 1.0f, 6.0f);
    bool ready = mgr.feed(pts, 2, info);

    CHECK(ready, "方位角回绕后帧就绪");
    if (ready) {
        auto frame = mgr.take();
        CHECK(frame.status == hal::LidarFrame::Status::kOk, "帧状态 kOk");
        CHECK(frame.pkt_got == 12, "积累了12包后触发");
    }
}

// =============================================================================
// 测试3：帧超时强制发布
// =============================================================================
static void test_timeout() {
    std::printf("\n[Test3] 帧超时强制发布\n");
    auto cfg = makeConfig();
    cfg.pkts_per_frame    = 1000;      // 包数不会触发
    cfg.frame_start_angle = 999.0f;    // 方位角不会触发
    cfg.frame_timeout_us  = 5000;      // 5ms 超时（测试用小值）
    hal::LidarFrameManager mgr(cfg);

    hal::LidarPoint pts[1];
    pts[0] = makePoint(10.0f, 0.0f, 0.0f);

    // 第一包设备时间 = 1000000us
    mgr.feed(pts, 1, makePkt(1000000, 10.0f, 12.0f));

    // 后续包设备时间超过 timeout：1000000 + 6000 > 1000000 + 5000
    bool ready = mgr.feed(pts, 1, makePkt(1000000 + 6000, 20.0f, 22.0f));

    CHECK(ready, "超时后帧就绪");
    if (ready) {
        auto frame = mgr.take();
        CHECK(frame.status == hal::LidarFrame::Status::kTimeout, "帧状态 kTimeout");
    }
    CHECK(mgr.stats().frame_timeout == 1, "frame_timeout 计数=1");
}

// =============================================================================
// 测试4：时间戳法丢包检测
// =============================================================================
static void test_drop_by_time() {
    std::printf("\n[Test4] 时间戳法丢包检测\n");
    auto cfg = makeConfig();
    cfg.pkts_per_frame    = 1000;
    cfg.frame_start_angle = 999.0f;
    cfg.pkt_gap_us        = 1000;   // 期望包间隔 1000us
    cfg.drop_ratio        = 1.8;    // 超过 1800us 视为丢包
    hal::LidarFrameManager mgr(cfg);

    hal::LidarPoint pts[1];
    pts[0] = makePoint(5.0f, 0.0f, 0.0f);

    // 包1
    mgr.feed(pts, 1, makePkt(1000000, 10.0f, 12.0f));
    // 包2：间隔 1000us（正好等于期望值，不触发丢包）
    mgr.feed(pts, 1, makePkt(1001000, 12.0f, 14.0f));
    // 包3：间隔 4000us（4.0×1000 > 1.8×1000=1800，估算丢了3包）
    mgr.feed(pts, 1, makePkt(1005000, 14.0f, 16.0f));

    const auto &s = mgr.stats();
    CHECK(s.gap_curr_us == 4000, "gap_curr 正确");
    // 估算：(4000 + 500) / 1000 - 1 = 4 - 1 = 3 包
    CHECK(s.drop_by_time == 3, "drop_by_time 估算=3");
}

// =============================================================================
// 测试5：序列号法丢包检测
// =============================================================================
static void test_drop_by_seq() {
    std::printf("\n[Test5] 序列号法丢包检测\n");
    auto cfg = makeConfig();
    cfg.pkts_per_frame    = 1000;
    cfg.frame_start_angle = 999.0f;
    cfg.has_seq           = true;
    hal::LidarFrameManager mgr(cfg);

    hal::LidarPoint pts[1];
    pts[0] = makePoint(5.0f, 0.0f, 0.0f);

    // seq: 0,1,2 → 正常
    mgr.feed(pts, 1, makePkt(1000000, 10.0f, 12.0f, 0, true));
    mgr.feed(pts, 1, makePkt(1001666, 12.0f, 14.0f, 1, true));
    mgr.feed(pts, 1, makePkt(1003332, 14.0f, 16.0f, 2, true));
    // seq 跳到 6（丢了 3,4,5 共3包）
    mgr.feed(pts, 1, makePkt(1004998, 16.0f, 18.0f, 6, true));

    CHECK(mgr.stats().drop_by_seq == 3, "drop_by_seq 确认=3");
}

// =============================================================================
// 测试6：无效点过滤（距离越界）
// =============================================================================
static void test_invalid_point_filter() {
    std::printf("\n[Test6] 无效点过滤\n");
    auto cfg = makeConfig();
    cfg.pkts_per_frame    = 1;      // 1包即触发
    cfg.frame_start_angle = 999.0f;
    cfg.min_dist_m        = 0.5f;
    cfg.max_dist_m        = 10.0f;
    hal::LidarFrameManager mgr(cfg);

    hal::LidarPoint pts[4];
    pts[0] = makePoint(0.0f, 0.0f, 0.0f);  // 距离=0, 无效
    pts[1] = makePoint(5.0f, 0.0f, 0.0f);  // 距离=5m, 有效
    pts[2] = makePoint(0.1f, 0.0f, 0.0f);  // 距离=0.1<0.5, 无效
    pts[3] = makePoint(50.0f, 0.0f, 0.0f); // 距离=50>10, 无效

    auto info = makePkt(1000000, 10.0f, 12.0f);
    bool ready = mgr.feed(pts, 4, info);

    CHECK(ready, "1包后帧就绪");
    if (ready) {
        auto frame = mgr.take();
        CHECK(frame.num_points == 1, "只有1个有效点（距离5m）");
    }
}

// =============================================================================
// 测试7：角度覆盖率
// =============================================================================
static void test_angle_coverage() {
    std::printf("\n[Test7] 角度覆盖率\n");
    auto cfg = makeConfig();
    cfg.pkts_per_frame    = 3;
    cfg.frame_start_angle = 999.0f;
    hal::LidarFrameManager mgr(cfg);

    hal::LidarPoint pts[1];
    pts[0] = makePoint(5.0f, 0.0f, 0.0f);

    // 覆盖 [0°,59°]：60度 / 360度 ≈ 16.7%
    mgr.feed(pts, 1, makePkt(1000000, 0.0f, 19.0f));
    mgr.feed(pts, 1, makePkt(1001666, 20.0f, 39.0f));
    bool ready = mgr.feed(pts, 1, makePkt(1003332, 40.0f, 59.0f));

    CHECK(ready, "3包后帧就绪");
    if (ready) {
        auto frame = mgr.take();
        // 覆盖 0~59°，60个度 → 60/360 ≈ 0.167
        const float expected = 60.0f / 360.0f;
        const float diff = std::fabs(frame.angle_cover - expected);
        CHECK(diff < 0.02f, "角度覆盖率约16.7%");
    }
}

// =============================================================================
// 测试8：三层时间戳滚动是否正确
// =============================================================================
static void test_timestamp_rolling() {
    std::printf("\n[Test8] 三层时间戳滚动\n");
    auto cfg = makeConfig();
    cfg.pkts_per_frame    = 1000;
    cfg.frame_start_angle = 999.0f;
    hal::LidarFrameManager mgr(cfg);

    hal::LidarPoint pts[1];
    pts[0] = makePoint(5.0f, 0.0f, 0.0f);

    mgr.feed(pts, 1, makePkt(100, 0.0f, 1.0f));
    mgr.feed(pts, 1, makePkt(200, 1.0f, 2.0f));
    mgr.feed(pts, 1, makePkt(400, 2.0f, 3.0f));

    const auto &s = mgr.stats();
    CHECK(s.time_curr_us  == 400, "time_curr=400");
    CHECK(s.time_prev_us  == 200, "time_prev=200");
    CHECK(s.time_older_us == 100, "time_older=100");
    CHECK(s.gap_curr_us   == 200, "gap_curr=200");
    CHECK(s.gap_prev_us   == 100, "gap_prev=100");
}

// =============================================================================
// 测试9：reset() 后状态清零
// =============================================================================
static void test_reset() {
    std::printf("\n[Test9] reset 清空状态\n");
    auto cfg = makeConfig();
    cfg.pkts_per_frame    = 2;
    cfg.frame_start_angle = 999.0f;
    hal::LidarFrameManager mgr(cfg);

    hal::LidarPoint pts[1];
    pts[0] = makePoint(5.0f, 0.0f, 0.0f);

    mgr.feed(pts, 1, makePkt(100, 0.0f, 1.0f));
    mgr.feed(pts, 1, makePkt(200, 1.0f, 2.0f)); // 触发一帧
    mgr.take();

    mgr.reset();
    const auto &s = mgr.stats();
    CHECK(s.frame_total  == 0, "reset后 frame_total=0");
    CHECK(s.pkt_total    == 0, "reset后 pkt_total=0");
    CHECK(s.time_curr_us == 0, "reset后 time_curr=0");
    CHECK(s.fps          == 0.0f, "reset后 fps=0");
}

// =============================================================================
// 测试10：statsReport 字符串非空
// =============================================================================
static void test_report_not_empty() {
    std::printf("\n[Test10] statsReport 字符串格式\n");
    auto cfg = makeConfig();
    cfg.pkts_per_frame    = 1;
    cfg.frame_start_angle = 999.0f;
    hal::LidarFrameManager mgr(cfg);

    hal::LidarPoint pts[1];
    pts[0] = makePoint(5.0f, 0.0f, 0.0f);
    mgr.feed(pts, 1, makePkt(100, 0.0f, 1.0f));
    mgr.take();

    const std::string r = mgr.report();
    CHECK(!r.empty(), "report 非空");
    CHECK(r.find("frames=") != std::string::npos, "report 含 frames=");
    CHECK(r.find("fps=")    != std::string::npos, "report 含 fps=");
    std::printf("  report: %s\n", r.c_str());
}

// =============================================================================
// main
// =============================================================================
int main() {
    std::printf("========================================\n");
    std::printf("  LidarFrameManager Unit Test\n");
    std::printf("========================================\n");

    test_count_trigger();
    test_azimuth_boundary();
    test_timeout();
    test_drop_by_time();
    test_drop_by_seq();
    test_invalid_point_filter();
    test_angle_coverage();
    test_timestamp_rolling();
    test_reset();
    test_report_not_empty();

    std::printf("\n========================================\n");
    std::printf("  结果: %d PASS  %d FAIL\n", g_pass, g_fail);
    std::printf("========================================\n");

    return (g_fail == 0) ? 0 : 1;
}
