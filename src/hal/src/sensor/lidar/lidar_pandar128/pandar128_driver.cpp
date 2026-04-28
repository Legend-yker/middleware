#include "sensor/lidar/lidar_pandar128/pandar128_driver.h"
#include "sensor/lidar/common/lidar_frame.h"
#include "hal_log.h"

// Hesai SDK 协议头依赖 little_to_native/big_to_native，但 plat_utils.h 里未定义。
// x86 Linux 是小端序，little_to_native 是恒等变换；big_to_native 做字节交换。
// 在包含协议头之前先定义，避免编译错误。
#include <cstdint>
namespace hesai { namespace lidar {
    template<typename T> inline T little_to_native(T v) { return v; }
    template<typename T> inline T big_to_native(T v) {
        if constexpr (sizeof(T) == 8) return static_cast<T>(__builtin_bswap64(
            static_cast<std::uint64_t>(v)));
        if constexpr (sizeof(T) == 4) return static_cast<T>(__builtin_bswap32(
            static_cast<std::uint32_t>(v)));
        if constexpr (sizeof(T) == 2) return static_cast<T>(__builtin_bswap16(
            static_cast<std::uint16_t>(v)));
        return v;
    }
    // GetMicroLidarTimeU64 需要此结构体保存 UTC 缓存状态
    struct LastUtcTime {
        std::uint8_t  last_utc[6]{};
        std::uint64_t last_time{0};
    };
}} // namespace hesai::lidar

// Hesai 真实协议头
#include "sdk_core/libhesai/UdpProtocol/udp_protocol_header.h"
#include "sdk_core/libhesai/UdpProtocol/udp_protocol_v1_4.h"

#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstring>
#include <ctime>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

// ── Hesai 真实协议常量 ────────────────────────────────────────────────────────
namespace {

constexpr std::size_t kMaxUdpBufSize = 10000; // UDP 接收缓冲区（字节）
constexpr float         kDistUnit        = 0.004f;   // 默认距离单位：4mm/单位
constexpr float         kAzimuthUnit     = 0.01f;    // 方位角单位：0.01°/单位

// 从 Hesai 包尾 UTC + 微秒偏移计算绝对时间（微秒，Unix epoch）
// 包尾 m_u8UTC[6]：year/month/day/hour/min/sec（UTC）
// GetTimestamp() 返回当前秒内的微秒偏移
std::uint64_t calcDeviceTime(const hesai::lidar::HS_LIDAR_TAIL_ME_V4 &tail) {
    const auto &u = tail.m_u8UTC;
    if (u[0] == 0 && u[1] == 0 && u[2] == 0) {
        // UTC 未同步，直接返回微秒偏移（仅相对时间，用于间隔计算）
        return static_cast<std::uint64_t>(tail.GetTimestamp());
    }
    struct tm t{};
    // Hesai 年字段：< 70 表示 20xx 年，>= 70 表示 19xx 年
    t.tm_year  = (u[0] < 70) ? (u[0] + 100) : u[0];
    t.tm_mon   = u[1] - 1;   // 月份从 0 开始
    t.tm_mday  = u[2];
    t.tm_hour  = u[3];
    t.tm_min   = u[4];
    t.tm_sec   = u[5];
    t.tm_isdst = 0;
    // timegm 把 struct tm（UTC）转为 Unix 时间戳，无时区影响
    const time_t unix_sec = timegm(&t);
    if (unix_sec < 0)
        return static_cast<std::uint64_t>(tail.GetTimestamp());
    return static_cast<std::uint64_t>(unix_sec) * 1000000ULL +
           static_cast<std::uint64_t>(tail.GetTimestamp());
}

// 解析结果：一包里提取的所有信息
struct ParseResult {
    hal::LidarPoint pts[256];  // 最多 2 Block × 128 通道
    std::uint32_t   num_pts{0};
    float           angle_start{0.0f};
    float           angle_end{0.0f};
    std::uint64_t   device_time_us{0};
    std::uint32_t   seq{0};
    bool            has_seq{false};
    bool            valid{false};
};

// 解析一个真实 Hesai UDP 包
ParseResult parseHesaiPacket(const std::uint8_t *buf, std::size_t len) {
    ParseResult result;

    // Step1：校验包头魔数
    if (len < sizeof(hesai::lidar::HS_LIDAR_PRE_HEADER) +
                  sizeof(hesai::lidar::HS_LIDAR_HEADER_ME_V4))
        return result;

    const auto *pre = reinterpret_cast<const hesai::lidar::HS_LIDAR_PRE_HEADER *>(buf);
    if (!pre->IsValidDelimiter())
        return result; // 魔数不对，丢弃

    // Step2：读取 Header（激光数、Block数、回波数、距离单位）
    const auto *hdr = reinterpret_cast<const hesai::lidar::HS_LIDAR_HEADER_ME_V4 *>(
        buf + sizeof(hesai::lidar::HS_LIDAR_PRE_HEADER));

    const std::uint8_t laser_num = hdr->GetLaserNum();   // 128
    const std::uint8_t block_num = hdr->GetBlockNum();   // 通常 2
    const float        dist_unit = (hdr->m_u8DistUnit > 0)
                                       ? hdr->m_u8DistUnit / 1000.0f
                                       : kDistUnit;

    if (laser_num == 0 || block_num == 0) return result;

    // Step3：逐 Block 解析点云
    // 内存布局：PreHeader | Header | [Block0: Azimuth + laser_num × Unit] × block_num | CRC | Tail | ...
    const std::uint8_t *body_ptr =
        buf + sizeof(hesai::lidar::HS_LIDAR_PRE_HEADER) +
        sizeof(hesai::lidar::HS_LIDAR_HEADER_ME_V4);

    float first_azimuth = 0.0f;
    float last_azimuth  = 0.0f;
    const std::uint32_t unit_size = hdr->unitSize();

    for (std::uint8_t b = 0; b < block_num; ++b) {
        // 读取本 Block 的方位角
        const auto *az_hdr =
            reinterpret_cast<const hesai::lidar::HS_LIDAR_BODY_AZIMUTH_ME_V4 *>(body_ptr);
        const float azimuth = az_hdr->GetAzimuth() * kAzimuthUnit; // 转成度

        if (b == 0)             first_azimuth = azimuth;
        if (b == block_num - 1) last_azimuth  = azimuth;

        // 读取 128 个通道的距离和反射率
        const std::uint8_t *unit_ptr =
            body_ptr + sizeof(hesai::lidar::HS_LIDAR_BODY_AZIMUTH_ME_V4);

        for (std::uint8_t ch = 0; ch < laser_num && result.num_pts < 256; ++ch) {
            const auto *unit =
                reinterpret_cast<const hesai::lidar::HS_LIDAR_BODY_CHN_UNIT_NO_CONF_ME_V4 *>(
                    unit_ptr + ch * unit_size);

            const float dist_m = unit->GetDistance() * dist_unit;
            if (dist_m < 0.01f) continue; // 距离为 0 的点跳过

            // 将极坐标（距离 + 方位角）转换为笛卡尔坐标 (x,y,z)
            // 注意：真实转换需要垂直角查表（仰角标定），这里用 ch 号近似
            // 生产环境应替换为厂商提供的角度标定表
            const float vert_angle_rad = (ch - laser_num / 2.0f) *
                                         (30.0f / laser_num) * 3.14159265f / 180.0f;
            const float horiz_rad = azimuth * 3.14159265f / 180.0f;
            const float horiz_dist = dist_m * std::cos(vert_angle_rad);

            hal::LidarPoint &pt = result.pts[result.num_pts++];
            pt.x         = horiz_dist * std::sin(horiz_rad);
            pt.y         = horiz_dist * std::cos(horiz_rad);
            pt.z         = dist_m    * std::sin(vert_angle_rad);
            pt.intensity = unit->GetReflectivity();
            pt.ring      = ch;
        }

        // 移动到下一个 Block
        body_ptr += sizeof(hesai::lidar::HS_LIDAR_BODY_AZIMUTH_ME_V4) +
                    unit_size * laser_num;
    }

    // Step4：跳过 Body CRC，读 Tail
    body_ptr += sizeof(hesai::lidar::HS_LIDAR_BODY_CRC_ME_V4);

    // 跳过 FunctionSafety（如果有）
    if (hesai::lidar::hasFunctionSafety(hdr->m_u8Status))
        body_ptr += sizeof(hesai::lidar::HS_LIDAR_FUNC_SAFETY_ME_V4);

    // 边界检查
    if (body_ptr + sizeof(hesai::lidar::HS_LIDAR_TAIL_ME_V4) > buf + len)
        return result;

    const auto *tail =
        reinterpret_cast<const hesai::lidar::HS_LIDAR_TAIL_ME_V4 *>(body_ptr);
    result.device_time_us = calcDeviceTime(*tail);
    body_ptr += sizeof(hesai::lidar::HS_LIDAR_TAIL_ME_V4);

    // Step5：读序列号（可选字段）
    if (hesai::lidar::hasSeqNum(hdr->m_u8Status) &&
        body_ptr + sizeof(hesai::lidar::HS_LIDAR_TAIL_SEQ_NUM_ME_V4) <= buf + len) {
        const auto *seq_hdr =
            reinterpret_cast<const hesai::lidar::HS_LIDAR_TAIL_SEQ_NUM_ME_V4 *>(body_ptr);
        result.seq     = seq_hdr->GetSeqNum();
        result.has_seq = true;
    }

    result.angle_start = first_azimuth;
    result.angle_end   = last_azimuth;
    result.valid       = true;
    return result;
}

// 根据转速计算每帧期望包数和包间隔
// Pandar128: 2 Block/包，128通道，以 rpm 旋转
// 每秒旋转圈数 = rpm/60，包发送速率约 rpm/60 × 单圈包数
// 单圈包数 ≈ 360° / (每包覆盖角度)，每包覆盖 2 Block，每 Block 对应若干度
// 实际参考值：600rpm → 约 600包/s，60包/帧（10Hz）
void calcFrameParams(std::uint32_t rpm,
                     std::uint32_t &pkts_per_frame,
                     std::uint64_t &pkt_gap_us) {
    const float hz       = static_cast<float>(rpm) / 60.0f; // 帧率
    const float total_pkt_rate = hz * 600.0f;                // 近似包速率（实测约 600/s@600rpm）
    pkts_per_frame = static_cast<std::uint32_t>(total_pkt_rate / hz);
    pkt_gap_us     = static_cast<std::uint64_t>(1e6f / total_pkt_rate);
}

} // namespace

namespace hal {

Pandar128Driver::Pandar128Driver() = default;

Pandar128Driver::~Pandar128Driver() {
    if (running_)
        doStopCapture();
}

bool Pandar128Driver::doInitParams(const nlohmann::json &configEntry) {
    if (!LidarBase::doInitParams(configEntry))
        return false;

    const auto &params = configEntry.value("params", nlohmann::json::object());
    if (data_port_ == 0)
        data_port_ = 2368; // Hesai 默认数据端口

    gps_port_    = static_cast<std::uint16_t>(params.value("gps_port", 10110));
    dual_return_ = params.value("dual_return", false);
    rpm_         = params.value("rpm", 600u);

    SLOG_INFO("init params ip=" + ip_ +
              " port=" + std::to_string(data_port_) +
              " rpm=" + std::to_string(rpm_) +
              " dual_return=" + (dual_return_ ? "true" : "false"));
    return true;
}

bool Pandar128Driver::doStartCapture() {
    if (!openSocket())
        return false;

    // 根据转速初始化 FrameManager
    std::uint32_t pkts_per_frame = 60;
    std::uint64_t pkt_gap_us     = 1666;
    calcFrameParams(rpm_, pkts_per_frame, pkt_gap_us);

    LidarFrameManager::Config cfg{};
    cfg.pkts_per_frame    = pkts_per_frame;
    cfg.frame_timeout_us  = static_cast<std::uint64_t>(
        1e6f / (rpm_ / 60.0f) * 1.5f);  // 1.5 倍帧周期
    cfg.max_points        = LidarFrame::kMaxPoints;
    cfg.pkt_gap_us        = pkt_gap_us;
    cfg.drop_ratio        = 1.8;
    cfg.has_seq           = true;         // Pandar128 支持序列号
    cfg.frame_start_angle = 0.0f;
    cfg.angle_tol         = 2.0f;
    cfg.min_dist_m        = 0.1f;
    cfg.max_dist_m        = 200.0f;

    frame_mgr_ = std::make_unique<LidarFrameManager>(cfg);

    SLOG_INFO("start capture port=" + std::to_string(data_port_) +
              " rpm=" + std::to_string(rpm_) +
              " pkts_per_frame=" + std::to_string(pkts_per_frame) +
              " pkt_gap_us=" + std::to_string(pkt_gap_us));

    frame_seq_ = 0;
    running_   = true;
    recv_thread_ = hal::utils::SingleThread(
        "pandar128_recv", [this] { udpRecvLoop(); });
    return true;
}

int Pandar128Driver::doStopCapture() {
    running_ = false;
    closeSocket();
    recv_thread_.wait();

    if (frame_mgr_) {
        SLOG_INFO("stop capture frames=" + std::to_string(frame_seq_) +
                  " " + frame_mgr_->report());
    }
    frame_mgr_.reset();
    return 0;
}

bool Pandar128Driver::openSocket() {
    closeSocket();

    sock_fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (sock_fd_ < 0) {
        SLOG_ERROR("socket failed errno=" + std::to_string(errno));
        return false;
    }

    int reuse = 1;
    ::setsockopt(sock_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    // 200ms 超时，方便 stop 时线程能及时退出
    timeval tv{};
    tv.tv_sec  = 0;
    tv.tv_usec = 200000;
    ::setsockopt(sock_fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons(data_port_);

    if (::bind(sock_fd_, reinterpret_cast<const sockaddr *>(&addr),
               sizeof(addr)) != 0) {
        SLOG_ERROR("bind failed port=" + std::to_string(data_port_) +
                   " errno=" + std::to_string(errno));
        closeSocket();
        return false;
    }

    SLOG_INFO("UDP socket bound port=" + std::to_string(data_port_));
    return true;
}

int Pandar128Driver::closeSocket() {
    if (sock_fd_ >= 0) {
        ::close(sock_fd_);
        sock_fd_ = -1;
    }
    return 0;
}

void Pandar128Driver::udpRecvLoop() {
    std::uint8_t recv_buf[kMaxUdpBufSize];
    SLOG_INFO("udpRecvLoop start");

    while (running_.load(std::memory_order_relaxed)) {
        // Step1：从 UDP socket 收一包
        const ssize_t recv_len =
            ::recvfrom(sock_fd_, recv_buf, sizeof(recv_buf), 0, nullptr, nullptr);

        if (recv_len < 0) {
            if (!running_.load(std::memory_order_relaxed)) break;
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                continue; // 超时或被信号打断，正常继续
            SLOG_WARN("recvfrom error errno=" + std::to_string(errno));
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }

        // Step2：记录系统收包时间（recvfrom 返回后立刻打，延迟最小）
        const std::uint64_t recv_time_us = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());

        // Step3：解析真实 Hesai 协议
        const ParseResult pr = parseHesaiPacket(
            recv_buf, static_cast<std::size_t>(recv_len));

        if (!pr.valid) {
            SLOG_WARN("invalid hesai packet len=" + std::to_string(recv_len));
            continue;
        }

        // Step4：填写 PktInfo 喂给 FrameManager
        LidarFrameManager::PktInfo info{};
        info.device_time_us = pr.device_time_us;
        info.recv_time_us   = recv_time_us;
        info.angle_start    = pr.angle_start;
        info.angle_end      = pr.angle_end;
        info.seq            = pr.seq;
        info.has_seq        = pr.has_seq;

        // Step5：组帧，有帧就绪则发布
        if (frame_mgr_->feed(pr.pts, pr.num_pts, info)) {
            LidarFrame frame = frame_mgr_->take();
            const int rc = publish(&frame, sizeof(frame));
            if (rc == 0) {
                ++frame_seq_;
                SLOG_DEBUG("frame published id=" + std::to_string(frame.frame_id) +
                           " pts=" + std::to_string(frame.num_points) +
                           " " + frame_mgr_->report());
            } else {
                SLOG_ERROR("publish failed rc=" + std::to_string(rc));
            }
        }
    }

    SLOG_INFO("udpRecvLoop exit total_frames=" + std::to_string(frame_seq_));
}

} // namespace hal
