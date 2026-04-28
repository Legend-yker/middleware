#include "sensor/lidar/common/lidar_frame_manager.h"

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
    // pkt_total > 0 才有"上一包"可以比较
    rollTime(info);
    if (stats_.pkt_total > 0) {
        checkDropByTime();
        if (info.has_seq && cfg_.has_seq)
            checkDropBySeq(info.seq);
    }
    stats_.pkt_total++;

    // Step2：方位角帧边界检测
    // 边界检测在 curr_pkts_ 自增之前，避免边界包被计入上一帧的 pkt_got
    if (curr_pkts_ > 0 && isNewFrame(info.angle_start)) {
        // 关闭上一帧（不含本包），本包将成为新帧的第一包
        closeFrame(LidarFrame::Status::kOk);
    }

    // Step3：记录新帧起始信息（每帧第一包时执行）
    if (curr_pkts_ == 0) {
        frame_start_us_ = info.device_time_us;
        frame_recv_us_  = info.recv_time_us;
        std::memset(angle_map_, 0, sizeof(angle_map_));
    }
    stats_.curr_pkts = ++curr_pkts_;

    // Step4：过滤无效点，追加到帧缓冲
    for (std::uint32_t i = 0; i < count; ++i) {
        if (!isGoodPoint(pts[i])) continue;
        if (buf_.num_points >= cfg_.max_points) {
            if (!ready_) closeFrame(LidarFrame::Status::kFull);
            break;
        }
        buf_.points[buf_.num_points++] = pts[i];
    }

    // Step5：更新角度覆盖位图
    markAngle(info.angle_start, info.angle_end);
    last_angle_ = info.angle_end;

    // Step6：超时检测（边界触发后已 ready_，不再重复触发）
    if (!ready_ && frame_start_us_ > 0 &&
        info.device_time_us - frame_start_us_ > cfg_.frame_timeout_us) {
        closeFrame(LidarFrame::Status::kTimeout);
    }

    // Step7：包数达到期望值（备用触发）
    if (!ready_ && curr_pkts_ >= cfg_.pkts_per_frame) {
        closeFrame(LidarFrame::Status::kOk);
    }

    stats_.curr_cover = calcCover();
    return ready_;
}

// ── take：取走就绪的帧（out_frame_，不是积累中的 buf_）──────────────────────
LidarFrame LidarFrameManager::take() {
    LidarFrame out = out_frame_;
    out_frame_ = LidarFrame{};
    ready_     = false;
    return out;
}

// ── 重置所有状态 ──────────────────────────────────────────────────────────────
void LidarFrameManager::reset() {
    stats_           = Stats{};
    out_frame_       = LidarFrame{};
    seq_initialized_ = false;
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
    if (!seq_initialized_) {
        // 第一次有序列号数据：只记录，不判断跳变
        stats_.last_seq  = seq;
        seq_initialized_ = true;
        return;
    }
    // 序列号 32bit 环绕，差值运算自动处理溢出
    const std::uint32_t expected = stats_.last_seq + 1u;
    if (seq != expected) {
        const std::uint32_t gap = seq - expected;
        if (gap < 1000u)   // 超过 1000 视为驱动重启，不计丢包
            stats_.drop_by_seq += gap;
    }
    stats_.last_seq = seq;
}

bool LidarFrameManager::isNewFrame(float angle) const {
    if (last_angle_ < 0.0f) return false;

    // 帧边界判断：上一包角度在 270°~360° 区间，当前包角度在 0°~90° 区间
    // 这意味着扫描从接近 360° 回绕到接近 0°，即完成了一圈
    // 使用固定区间比"frame_start_angle ± tol"更稳定，避免 tol 设置不当误触发
    const bool prev_in_tail    = (last_angle_ >= 270.0f);
    const bool curr_in_head    = (angle       <  90.0f);
    return prev_in_tail && curr_in_head;
}

void LidarFrameManager::closeFrame(LidarFrame::Status s) {
    // 把积累好的帧元信息写入，然后搬到 out_frame_（等待 take()）
    buf_.frame_id      = frame_id_++;
    buf_.time_first_us = frame_start_us_;
    buf_.time_last_us  = stats_.time_curr_us;
    buf_.time_recv_us  = frame_recv_us_;
    buf_.pkt_got       = curr_pkts_;
    buf_.pkt_want      = cfg_.pkts_per_frame;
    buf_.drop_by_time  = stats_.drop_by_time;
    buf_.drop_by_seq   = stats_.drop_by_seq;
    buf_.status        = s;
    buf_.angle_cover   = calcCover();

    out_frame_ = buf_;   // 完成的帧交给 out_frame_

    stats_.frame_total++;
    switch (s) {
    case LidarFrame::Status::kOk:      stats_.frame_ok++;     break;
    case LidarFrame::Status::kTimeout: stats_.frame_timeout++; break;
    case LidarFrame::Status::kFull:    stats_.frame_full++;    break;
    }

    const std::uint64_t now_us = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    calcFps(now_us);

    // 清空积累缓冲，准备下一帧
    clearBuf();
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
