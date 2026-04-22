#include "sensor/lidar/lidar_pandar128/pandar128_driver.h"
#include "sensor/lidar/common/lidar_frame.h"
#include "common/crc32.h"
#include "hal_log.h"          // ← SLOG_* 宏

#include <arpa/inet.h>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <cerrno>
#include <netinet/in.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace {

// fake Pandar 协议头：
// 当前阶段先用最小可控协议把链路打通，后续再替换成真实厂商协议。
constexpr std::uint32_t kFakePandarMagic   = 0x50414E44u;
constexpr std::uint16_t kFakePandarVersion = 1u;

struct FakePandarPoint {
    float         x;
    float         y;
    float         z;
    std::uint8_t  intensity;
    std::uint8_t  ring;
    std::uint16_t reserved;
};

struct FakePandarPacket {
    std::uint32_t  magic;
    std::uint16_t  version;
    std::uint16_t  point_count;
    std::uint32_t  frame_id;
    std::uint64_t  timestamp_us;
    FakePandarPoint points[hal::LidarFrame::kMaxDemoPoints];
    std::uint32_t  payload_crc32;
};

static_assert(sizeof(FakePandarPoint) == 16);

bool decodePacket(const std::uint8_t *buf, std::size_t len, hal::LidarFrame &frame) {
    // 模块 1：协议校验
    // 检查长度、魔数、版本、点数、CRC，确保是合法的 fake UDP 包。
    if (len != sizeof(FakePandarPacket))
        return false;

    FakePandarPacket packet{};
    std::memcpy(&packet, buf, sizeof(packet));

    if (packet.magic != kFakePandarMagic || packet.version != kFakePandarVersion)
        return false;
    if (packet.point_count > hal::LidarFrame::kMaxDemoPoints)
        return false;

    const std::uint32_t expected_crc =
        dcms::crc32(&packet, offsetof(FakePandarPacket, payload_crc32));
    if (expected_crc != packet.payload_crc32)
        return false;

    // 模块 2：协议转内部数据结构
    // 把 UDP 包里的点云数据拷贝到 HAL 统一的 LidarFrame。
    frame = {};
    frame.frame_id     = packet.frame_id;
    frame.timestamp_us = packet.timestamp_us;
    frame.num_points   = packet.point_count;

    for (std::uint32_t i = 0; i < packet.point_count; ++i) {
        frame.points[i].x         = packet.points[i].x;
        frame.points[i].y         = packet.points[i].y;
        frame.points[i].z         = packet.points[i].z;
        frame.points[i].intensity = packet.points[i].intensity;
        frame.points[i].ring      = packet.points[i].ring;
    }

    return true;
}

} // namespace

namespace hal {

Pandar128Driver::Pandar128Driver() = default;

Pandar128Driver::~Pandar128Driver() {
    if (capturing_)
        doStopCapture();
}

bool Pandar128Driver::doInitParams(const nlohmann::json &configEntry) {
    if (!LidarBase::doInitParams(configEntry))
        return false;

    const auto &params = configEntry.value("params", nlohmann::json::object());
    if (data_port_ == 0)
        data_port_ = 2368;
    gps_port_    = static_cast<std::uint16_t>(params.value("gps_port", 10110));
    dual_return_ = params.value("dual_return", false);

    // 日志：传感器初始化参数已解析
    // 输出示例：[20260412_102536][I][MOD:hal][lidar:57] init params ip=192.168.1.201 port=2368
    SLOG_INFO("init params ip=" + ip_ +
              " port=" + std::to_string(data_port_) +
              " gps_port=" + std::to_string(gps_port_) +
              " dual_return=" + (dual_return_ ? "true" : "false"));
    return true;
}

bool Pandar128Driver::doStartCapture() {
    // 流程 1：启动时先打开并绑定 UDP 端口。
    if (!openSocket())
        return false;

    SLOG_INFO("start capture ip=" + ip_ +
              " data_port=" + std::to_string(data_port_) +
              " gps_port=" + std::to_string(gps_port_));

    frame_seq_  = 0;
    capturing_  = true;
    // 流程 2：启动后台线程持续收包。
    recv_thread_ = std::make_unique<std::thread>([this] { udpRecvLoop(); });
    return true;
}

int Pandar128Driver::doStopCapture() {
    capturing_ = false;
    closeSocket();
    if (recv_thread_ && recv_thread_->joinable())
        recv_thread_->join();
    recv_thread_.reset();

    SLOG_INFO("stop capture total_frames=" + std::to_string(frame_seq_));
    return 0;
}

bool Pandar128Driver::openSocket() {
    closeSocket();

    // 模块 3：UDP 接收端初始化。
    // UDP 是无连接协议，但接收端依然必须 socket + bind 才能监听端口。
    socket_fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (socket_fd_ < 0) {
        SLOG_ERROR("socket create failed errno=" + std::to_string(errno));
        return false;
    }

    int reuse = 1;
    if (::setsockopt(socket_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) != 0) {
        SLOG_ERROR("setsockopt SO_REUSEADDR failed errno=" + std::to_string(errno));
        closeSocket();
        return false;
    }

    // 给 recvfrom 设置 200ms 超时，避免 stop 时线程永久阻塞。
    timeval timeout{};
    timeout.tv_sec  = 0;
    timeout.tv_usec = 200000;
    if (::setsockopt(socket_fd_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0) {
        SLOG_ERROR("setsockopt SO_RCVTIMEO failed errno=" + std::to_string(errno));
        closeSocket();
        return false;
    }

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port        = htons(data_port_);

    if (::bind(socket_fd_, reinterpret_cast<const sockaddr *>(&addr), sizeof(addr)) != 0) {
        SLOG_ERROR("bind failed port=" + std::to_string(data_port_) +
                   " errno=" + std::to_string(errno));
        closeSocket();
        return false;
    }

    SLOG_INFO("UDP socket bound port=" + std::to_string(data_port_));
    return true;
}

int Pandar128Driver::closeSocket() {
    if (socket_fd_ >= 0) {
        ::close(socket_fd_);
        socket_fd_ = -1;
    }
    return 0;
}

int Pandar128Driver::udpRecvLoop() {
    std::uint8_t recv_buf[sizeof(FakePandarPacket)];

    SLOG_DEBUG("udpRecvLoop started");

    while (capturing_.load(std::memory_order_relaxed)) {
        LidarFrame frame{};

        // 流程 4：从本地 UDP 端口收包。
        const ssize_t recv_len =
            ::recvfrom(socket_fd_, recv_buf, sizeof(recv_buf), 0, nullptr, nullptr);
        if (recv_len < 0) {
            if (!capturing_.load(std::memory_order_relaxed))
                break;
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                continue; // 超时或被信号打断，正常继续
            SLOG_WARN("recvfrom failed errno=" + std::to_string(errno));
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }

        if (!decodePacket(recv_buf, static_cast<std::size_t>(recv_len), frame)) {
            SLOG_WARN("invalid fake udp packet len=" + std::to_string(recv_len));
            continue;
        }

        // 流程 5：把解析后的 LidarFrame 发布到 DCMS。
        const int rc = publish(&frame, sizeof(frame));
        if (rc == 0) {
            ++frame_seq_;
            SLOG_DEBUG("published frame_id=" + std::to_string(frame.frame_id) +
                       " pts=" + std::to_string(frame.num_points) +
                       " total=" + std::to_string(frame_seq_));
        } else {
            SLOG_ERROR("publish failed rc=" + std::to_string(rc) +
                       " frame_id=" + std::to_string(frame.frame_id));
        }
    }

    SLOG_DEBUG("udpRecvLoop exited");
    return 0;
}

} // namespace hal
