#pragma once

#include "sensor/lidar/common/lidar_frame.h"

#include <cstdint>
#include <string>

namespace hal {

// LidarFrameManager — 激光雷达组帧管理器
//
// 职责：
//   1. 将多个 UDP 包积累成一帧点云
//   2. 三层时间戳滚动，用两种方式检测丢包
//   3. 方位角回绕检测帧边界（0°→359°→0° 一圈=一帧）
//   4. 超时/点数溢出时强制发布不完整帧
//   5. 统计帧率、覆盖率、丢包率供日志打印
class LidarFrameManager {
public:
    // ── 初始化配置（驱动根据参数算好后传入）──────────────────────────────
    struct Config {
        // 组帧参数
        std::uint32_t pkts_per_frame;    // 一帧期望包数，如 60（600rpm/10Hz）
        std::uint64_t frame_timeout_us;  // 帧超时，超过此时间强制发布，如 150000us
        std::uint32_t max_points;        // 点数上限，超了提前发布，默认 kMaxPoints

        // 丢包检测（时间间隔法）
        std::uint64_t pkt_gap_us;        // 期望包间隔，如 1666us（600包/s）
        double        drop_ratio;        // 间隔超过 ratio 倍视为丢包，如 1.8

        // 丢包检测（序列号法）
        bool          has_seq;           // 硬件包里是否有序列号字段

        // 帧边界检测（方位角法）
        float         frame_start_angle; // 帧起始角度，通常为 0.0°
        float         angle_tol;         // 边界容差，如 2.0°

        // 无效点过滤
        float         min_dist_m;        // 小于此距离的点视为无效，如 0.1m
        float         max_dist_m;        // 大于此距离的点视为无效，如 200.0m
    };

    // ── 每个 UDP 包带进来的信息 ───────────────────────────────────────────
    struct PktInfo {
        std::uint64_t device_time_us;  // 设备时间戳（从包尾 Tail 解出）
        std::uint64_t recv_time_us;    // 系统收包时间（recvfrom 后立刻打）
        float         angle_start;     // 本包起始方位角（度，0~360）
        float         angle_end;       // 本包结束方位角（度，0~360）
        std::uint32_t seq;             // 序列号（没有则填 0）
        bool          has_seq;
    };

    // ── 统计数据 ──────────────────────────────────────────────────────────
    struct Stats {
        // 三层时间戳（核心）
        // 每收到一包：time_older←time_prev，time_prev←time_curr，time_curr←新包时间
        std::uint64_t time_curr_us{0};   // 当前包时间
        std::uint64_t time_prev_us{0};   // 上一包时间
        std::uint64_t time_older_us{0};  // 上上包时间
        std::uint64_t gap_curr_us{0};    // time_curr - time_prev（当前包间隔）
        std::uint64_t gap_prev_us{0};    // time_prev - time_older（上一包间隔）

        // 丢包累计
        std::uint32_t pkt_total{0};      // 总共收到多少包
        std::uint32_t drop_by_time{0};   // 时间间隔法估算的累计丢包数
        std::uint32_t drop_by_seq{0};    // 序列号法确认的累计丢包数
        std::uint32_t last_seq{0};       // 上一包序列号

        // 帧累计
        std::uint32_t frame_total{0};    // 总发出帧数
        std::uint32_t frame_ok{0};       // 正常帧（方位角完整）
        std::uint32_t frame_timeout{0};  // 超时强制发布的帧
        std::uint32_t frame_full{0};     // 点数溢出提前发布的帧

        // 帧率
        float         fps{0.0f};                // 当前实测帧率
        std::uint64_t last_frame_time_us{0};    // 上一帧发出时的系统时间

        // 当前帧进度
        std::uint32_t curr_pkts{0};      // 当前帧已收包数
        float         curr_cover{0.0f};  // 当前帧角度覆盖率（0.0~1.0）
    };

    explicit LidarFrameManager(const Config &cfg);
    ~LidarFrameManager() = default;

    LidarFrameManager(const LidarFrameManager &) = delete;
    LidarFrameManager &operator=(const LidarFrameManager &) = delete;

    // 喂入一个 UDP 包解析出的点云数据。
    // 返回 true 表示一帧已积累完成，调用 take() 取走帧。
    bool feed(const LidarPoint *pts, std::uint32_t count, const PktInfo &info);

    // feed() 返回 true 后调用，取走完整帧（调用后内部缓冲已清空）
    LidarFrame take();

    // 获取当前统计数据
    const Stats &stats() const { return stats_; }

    // 格式化一行统计字符串，供 slogInfo 每帧打印
    // 示例："frames=100 ok=98 timeout=2 drop_time=5 drop_seq=3 fps=9.8"
    std::string report() const;

    // 驱动重启时调用，清空所有状态
    void reset();

private:
    // 滚动三层时间戳，更新 gap_curr/gap_prev
    void rollTime(const PktInfo &info);

    // 时间间隔法丢包检测：gap_curr 超过 pkt_gap × drop_ratio 则估算丢包数
    void checkDropByTime();

    // 序列号法丢包检测：seq 不连续则确认丢包数
    void checkDropBySeq(std::uint32_t seq);

    // 判断是否到达新帧起点（方位角从高角度回绕到 frame_start_angle 附近）
    bool isNewFrame(float angle) const;

    // 完成当前帧：填写帧元信息，标记 ready_，准备被 take()
    void closeFrame(LidarFrame::Status s);

    // 清空帧缓冲，准备积累下一帧
    void clearBuf();

    // 判断点是否有效（距离范围内）
    bool isGoodPoint(const LidarPoint &p) const;

    // 将 [angle_start, angle_end] 标记到角度覆盖位图
    void markAngle(float start, float end);

    // 从覆盖位图计算覆盖率（0.0~1.0）
    float calcCover() const;

    // 更新实测帧率
    void calcFps(std::uint64_t now_us);

    Config      cfg_;
    Stats       stats_{};

    LidarFrame  buf_{};          // 正在积累的帧（未完成）
    LidarFrame  out_frame_{};    // 已完成等待 take() 取走的帧
    std::uint32_t curr_pkts_{0};         // 当前帧已收包数
    std::uint64_t frame_start_us_{0};    // 当前帧第一包的设备时间
    std::uint64_t frame_recv_us_{0};     // 当前帧系统收包时间（用于 time_recv_us）
    float         last_angle_{-1.0f};    // 上一包结束方位角（帧边界检测用）
    bool          ready_{false};         // 是否有帧等待 take()
    bool          seq_initialized_{false}; // 序列号是否已初始化
    std::uint32_t frame_id_{0};          // 帧序号（单调递增）

    // 角度覆盖位图：360 bit，每 1° 一个 bit，共 6 个 uint64
    // bit[i]=1 表示 i° 方向有点云数据
    std::uint64_t angle_map_[6]{};
};

} // namespace hal
