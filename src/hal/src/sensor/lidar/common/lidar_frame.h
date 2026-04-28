#pragma once

#include <cstdint>

namespace hal {

struct LidarPoint {
    float         x;
    float         y;
    float         z;
    std::uint8_t  intensity;
    std::uint8_t  ring;
    std::uint8_t  padding[2];
};
static_assert(sizeof(LidarPoint) == 16, "LidarPoint must be 16 bytes");

struct LidarFrame {
    // ── 帧标识 ──────────────────────────────────────────────────────────────
    std::uint32_t frame_id;         // 单调递增帧序号

    // ── 三个时间戳 ──────────────────────────────────────────────────────────
    // 关系：组帧耗时 = time_last_us - time_first_us
    //       传输延迟 = time_recv_us  - time_first_us
    std::uint64_t time_first_us;    // 帧内第一包的设备时间（从包尾 Tail 解出）
    std::uint64_t time_last_us;     // 帧内最后一包的设备时间
    std::uint64_t time_recv_us;     // HAL 收到第一包时的系统时间（steady_clock）

    // ── 点云 ────────────────────────────────────────────────────────────────
    std::uint32_t num_points;
    // Pandar128: 128通道 × 2回波 × ~120包/帧 = 30720 点上限
    static constexpr std::uint32_t kMaxPoints = 30720;
    LidarPoint points[kMaxPoints];

    // ── 丢包信息 ────────────────────────────────────────────────────────────
    std::uint32_t pkt_got;          // 本帧实际收到的包数
    std::uint32_t pkt_want;         // 本帧期望包数（由转速决定）
    std::uint32_t drop_by_time;     // 时间间隔法估算的丢包数
    std::uint32_t drop_by_seq;      // 序列号跳变确认的丢包数

    // ── 帧状态 ──────────────────────────────────────────────────────────────
    enum class Status : std::uint8_t {
        kOk      = 0,   // 方位角完整回绕，正常帧
        kTimeout = 1,   // 超时强制发布，末尾数据缺失
        kFull    = 2,   // 点数超上限提前发布
    };
    Status        status;
    std::uint8_t  pad[3];

    // ── 角度覆盖率 ──────────────────────────────────────────────────────────
    float         angle_cover;      // 0.0~1.0，本帧扫过了多少360°（1.0=完整）
};

} // namespace hal
