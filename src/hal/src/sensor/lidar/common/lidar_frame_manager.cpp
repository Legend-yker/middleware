#include "sensor/lidar/common/lidar_frame_manager.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstring>
#include <cmath>
#include <sstream>

namespace hal {

LidarFrameManager::LidarFrameManager(const Config &cfg) : cfg_(cfg) {
    assert(cfg_.pkts_per_frame > 0);
    assert(cfg_.pkt_gap_us > 0);
    assert(cfg_.drop_ratio > 1.0);
    clearBuf();
}

// ── 主入口：喂入一包数据 ──────────────────────────────────────────────────────
bool LidarFrameManager::feed(const LidarPoint *pts, std::uint32_t count,
                             const PktInfo &info) {
    ready_ = false;

    // Step1：更新三层时间戳 + 丢包检测
    rollTime(info);
    if (stats_.pkt_total > 1) {          // 第一包没有"上一包"，跳过检测
        checkDropByTime();
        if (info.has_seq && cfg_.has_seq)
            checkDropBySeq(info.seq);
    }
    stats_.pkt_total++;
    stats_.curr_pkts = ++curr_pkts_;

    // Step2：判断是否到达新帧边界（当前包起始角触发帧边界）
    // 必须在追加点之前判断，否则新帧第一包的点会被算进上一帧
    if (curr_pkts_ > 1 && isNewFrame(info.angle_start)) {
        closeFrame(LidarFrame::Status::kOk);
        return true;
    }

    // Step3：记录帧起始信息（第一包）
    if (curr_pkts_ == 1) {
        frame_start_us_  = info.device_time_us;
        frame_recv_us_   = info.recv_time_us;
        std::memset(angle_map_, 0, sizeof(angle_map_));
    }

    // Step4：过滤无效点，追加到帧缓冲
    for (std::uint32_t i = 0; i < count; ++i) {
        if (!isGoodPoint(pts[i]))
            continue;
        if (buf_.num_points >= cfg_.max_points) {
            // 点数溢出：提前发布
            closeFrame(LidarFrame::Status::kFull);
            return true;
        }
        buf_.points[buf_.num_points++] = pts[i];
    }

    // Step5：更新角度覆盖位图
    markAngle(info.angle_start, info.angle_end);
    last_angle_ = info.angle_end;

    // Step6：超时检测（用设备时间判断帧积累是否过久）
    if (frame_start_us_ > 0 &&
        info.device_time_us - frame_start_us_ > cfg_.frame_timeout_us) {
        closeFrame(LidarFrame::Status::kTimeout);
        return true;
    }

    // Step7：包数达到期望值（备用触发，方位角法为主）
    if (curr_pkts_ >= cfg_.pkts_per_frame) {
        closeFrame(LidarFrame::Status::kOk);
        return true;
    }

    stats_.curr_cover = calcCover();
    return false;
}

// ── take：取走就绪的帧 ────────────────────────────────────────────────────────
LidarFrame LidarFrameManager::take() {
    LidarFrame out = buf_;
    clearBuf();
    ready_ = false;
    return out;
}

// ── 重置所有状态 ──────────────────────────────────────────────────────────────
void LidarFrameManager::reset() {
    stats_  = Stats{};
    clearBuf();
    last_angle_ = -1.0f;
    ready_      = false;
    frame_id_   = 0;
}

// ── 格式化统计报告 ────────────────────────────────────────────────────────────
std::string LidarFrameManager::report() const {
    std::ostringstream ss;
    ss << "frames=" << stats_.frame_total
       << " ok="      << stats_.frame_ok
       << " timeout=" << stats_.frame_timeout
       << " full="    << stats_.frame_full
       << " drop_t="  << stats_.drop_by_time
       << " drop_s="  << stats_.drop_by_seq
       << " fps="     << static_cast<int>(stats_.fps * 10) / 10.0f
       << " pts="     << buf_.num_points
       << " cover="   << static_cast<int>(stats_.curr_cover * 100) << "%";
    return ss.str();
}

// ── 私有方法实现 ──────────────────────────────────────────────────────────────

void LidarFrameManager::rollTime(const PktInfo &info) {
    // 三层时间戳向前滚动：older ← prev ← curr ← 新包
    stats_.time_older_us = stats_.time_prev_us;
    stats_.time_prev_us  = stats_.time_curr_us;
    stats_.time_curr_us  = info.device_time_us;

    // 更新两个间隔
    stats_.gap_prev_us = stats_.gap_curr_us;
    if (stats_.time_prev_us > 0 && stats_.time_curr_us > stats_.time_prev_us)
        stats_.gap_curr_us = stats_.time_curr_us - stats_.time_prev_us;
}

void LidarFrameManager::checkDropByTime() {
    // 当前间隔超过期望间隔 × drop_ratio，说明中间有包丢了
    const auto threshold = static_cast<std::uint64_t>(
        cfg_.pkt_gap_us * cfg_.drop_ratio);
    if (stats_.gap_curr_us > threshold && cfg_.pkt_gap_us > 0) {
        // 估算丢了几包：(实际间隔 / 期望间隔) - 1，四舍五入
        const std::uint32_t dropped = static_cast<std::uint32_t>(
            (stats_.gap_curr_us + cfg_.pkt_gap_us / 2) / cfg_.pkt_gap_us) - 1u;
        stats_.drop_by_time += dropped;
    }
}

void LidarFrameManager::checkDropBySeq(std::uint32_t seq) {
    if (stats_.pkt_total == 1) {
        // 第一包，只记录序列号，不判断跳变
        stats_.last_seq = seq;
        return;
    }
    // 序列号是 32bit 环绕，用差值处理溢出
    const std::uint32_t expected = stats_.last_seq + 1u;
    if (seq != expected) {
        // 跳变：可能丢包，也可能驱动重启，取两者中较小的
        const std::uint32_t gap = seq - expected; // 32bit 环绕自动处理
        if (gap < 1000u)                          // 超过 1000 视为重启，不计丢包
            stats_.drop_by_seq += gap;
    }
    stats_.last_seq = seq;
}

bool LidarFrameManager::isNewFrame(float angle) const {
    if (last_angle_ < 0.0f)
        return false; // 还没有上一包，不判断

    // 检测方位角从高角度回绕到 frame_start_angle 附近
    // 典型情况：last_angle_ ≈ 359°，angle ≈ 0°
    const float start = cfg_.frame_start_angle;
    const float tol   = cfg_.angle_tol;

    // 上一包在帧终点附近（接近 360° 或 frame_start_angle 之前）
    const bool prev_near_end = (last_angle_ > 360.0f - tol * 4.0f) ||
                               (last_angle_ > start + 180.0f);
    // 当前包在帧起点附近
    const bool curr_near_start = (angle >= start - tol) &&
                                 (angle <= start + tol + 10.0f);

    return prev_near_end && curr_near_start;
}

void LidarFrameManager::closeFrame(LidarFrame::Status s) {
    buf_.frame_id     = frame_id_++;
    buf_.time_first_us = frame_start_us_;
    buf_.time_last_us  = stats_.time_curr_us;
    buf_.time_recv_us  = frame_recv_us_;
    buf_.pkt_got       = curr_pkts_;
    buf_.pkt_want      = cfg_.pkts_per_frame;
    buf_.drop_by_time  = stats_.drop_by_time; // 累计值，调用方可前后做差
    buf_.drop_by_seq   = stats_.drop_by_seq;
    buf_.status        = s;
    buf_.angle_cover   = calcCover();

    stats_.frame_total++;
    switch (s) {
    case LidarFrame::Status::kOk:      stats_.frame_ok++;      break;
    case LidarFrame::Status::kTimeout: stats_.frame_timeout++;  break;
    case LidarFrame::Status::kFull:    stats_.frame_full++;     break;
    }

    // 用系统时间计算帧率（比设备时间更稳定）
    const std::uint64_t now_us = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    calcFps(now_us);

    stats_.curr_pkts  = 0;
    stats_.curr_cover = 0.0f;
    ready_ = true;
}

void LidarFrameManager::clearBuf() {
    buf_             = LidarFrame{};
    buf_.num_points  = 0;
    curr_pkts_       = 0;
    frame_start_us_  = 0;
    frame_recv_us_   = 0;
    last_angle_      = -1.0f;
    std::memset(angle_map_, 0, sizeof(angle_map_));
}

bool LidarFrameManager::isGoodPoint(const LidarPoint &p) const {
    const float dist = std::sqrt(p.x * p.x + p.y * p.y + p.z * p.z);
    return dist >= cfg_.min_dist_m && dist <= cfg_.max_dist_m;
}

void LidarFrameManager::markAngle(float start, float end) {
    // 把 [start, end] 对应的度数（整数）在 angle_map_ 里置位
    // 处理回绕：若 start > end，说明跨过了 0°
    auto markDeg = [this](int deg) {
        deg = ((deg % 360) + 360) % 360; // 归一化到 [0, 360)
        angle_map_[deg / 64] |= (1ULL << (deg % 64));
    };

    const int i_start = static_cast<int>(start);
    const int i_end   = static_cast<int>(end);

    if (i_start <= i_end) {
        for (int d = i_start; d <= i_end && d < 360; ++d)
            markDeg(d);
    } else {
        // 跨越 0° 的情况
        for (int d = i_start; d < 360; ++d) markDeg(d);
        for (int d = 0; d <= i_end;   ++d) markDeg(d);
    }
}

float LidarFrameManager::calcCover() const {
    int bits = 0;
    for (auto v : angle_map_)
        bits += __builtin_popcountll(v);
    return static_cast<float>(bits) / 360.0f;
}

void LidarFrameManager::calcFps(std::uint64_t now_us) {
    if (stats_.last_frame_time_us > 0 && now_us > stats_.last_frame_time_us) {
        const float period_s = static_cast<float>(
            now_us - stats_.last_frame_time_us) / 1e6f;
        // 滑动平均：新值权重 0.3，历史权重 0.7，避免帧率抖动
        const float new_fps = (period_s > 0.0f) ? (1.0f / period_s) : 0.0f;
        stats_.fps = 0.7f * stats_.fps + 0.3f * new_fps;
    }
    stats_.last_frame_time_us = now_us;
}

} // namespace hal
