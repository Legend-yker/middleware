#include "sensor/lidar/lidar_pandar128/pandar128_driver.h"
#include "sensor/lidar/common/lidar_frame.h"
#include "common/crc32.h"
#include "common/dcms_msg.h"

#include <arpa/inet.h>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <errno.h>
#include <iostream>
#include <netinet/in.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace {

// fake Pandar 协议头：
// 当前阶段先用最小可控协议把链路打通，后续再替换成真实厂商协议。
constexpr std::uint32_t kFakePandarMagic = 0x50414E44u;
constexpr std::uint16_t kFakePandarVersion = 1u;

struct FakePandarPoint {
    float x;
    float y;
    float z;
    std::uint8_t intensity;
    std::uint8_t ring;
    std::uint16_t reserved;
};

struct FakePandarPacket {
    std::uint32_t magic;
    std::uint16_t version;
    std::uint16_t point_count;
    std::uint32_t frame_id;
    std::uint64_t timestamp_us;
    FakePandarPoint points[hal::LidarFrame::kMaxDemoPoints];
    std::uint32_t payload_crc32;
};

static_assert(sizeof(FakePandarPoint) == 16);

bool decodePacket(const std::uint8_t *buf, std::size_t len, hal::LidarFrame &frame) {
    // 模块 1：协议校验
    // 先检查长度、魔数、版本、点数、CRC，确保收到的是我们定义的 fake UDP 包。
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
    // 通过后，把 UDP 包里的点云数据拷贝到 HAL 统一的 LidarFrame。
    frame = {};
    frame.frame_id = packet.frame_id;
    frame.timestamp_us = packet.timestamp_us;
    frame.num_points = packet.point_count;

    for (std::uint32_t i = 0; i < packet.point_count; ++i) {
        frame.points[i].x = packet.points[i].x;
        frame.points[i].y = packet.points[i].y;
        frame.points[i].z = packet.points[i].z;
        frame.points[i].intensity = packet.points[i].intensity;
        frame.points[i].ring = packet.points[i].ring;
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
    gps_port_ = static_cast<std::uint16_t>(params.value("gps_port", 10110));
    dual_return_ = params.value("dual_return", false);
    return true;
}

bool Pandar128Driver::doStartCapture() {
    // 流程 1：启动时先打开并绑定 UDP 端口。
    if (!openSocket())
        return false;

    std::cout << "[Pandar128] start capture ip=" << ip_
              << " data_port=" << data_port_
              << " gps_port=" << gps_port_
              << " topic_id=0x" << std::hex << topicId() << std::dec << '\n';

    frame_seq_ = 0;
    capturing_ = true;
    // 流程 2：启动后台线程持续收包。
    recv_thread_ = std::make_unique<std::thread>([this] { udpRecvLoop(); });
    return true;
}

void Pandar128Driver::doStopCapture() {
    // 流程 3：停止时先让循环退出，再关闭 socket，最后回收线程。
    capturing_ = false;
    closeSocket();
    if (recv_thread_ && recv_thread_->joinable())
        recv_thread_->join();
    recv_thread_.reset();
    std::cout << "[Pandar128] stop capture, published " << frame_seq_ << " frames\n";
}

bool Pandar128Driver::openSocket() {
    closeSocket();

    // 模块 3：UDP 接收端初始化。
    // 注意：UDP 是无连接协议，但接收端依然必须有 socket 并 bind 到本地端口。
    socket_fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (socket_fd_ < 0) {
        std::cerr << "[Pandar128] socket create failed errno=" << errno << '\n';
        return false;
    }

    int reuse = 1;
    if (::setsockopt(socket_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) != 0) {
        std::cerr << "[Pandar128] setsockopt failed errno=" << errno << '\n';
        closeSocket();
        return false;
    }

    timeval timeout{};
    timeout.tv_sec = 0;
    timeout.tv_usec = 200000;
    // 给 recvfrom 设置超时，避免 stop 时线程永久阻塞。
    if (::setsockopt(socket_fd_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0) {
        std::cerr << "[Pandar128] setsockopt timeout failed errno=" << errno << '\n';
        closeSocket();
        return false;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(data_port_);

    if (::bind(socket_fd_, reinterpret_cast<const sockaddr *>(&addr), sizeof(addr)) != 0) {
        std::cerr << "[Pandar128] bind failed port=" << data_port_
                  << " errno=" << errno << '\n';
        closeSocket();
        return false;
    }

    return true;
}

void Pandar128Driver::closeSocket() {
    if (socket_fd_ >= 0) {
        ::close(socket_fd_);
        socket_fd_ = -1;
    }
}

void Pandar128Driver::udpRecvLoop() {
    std::uint8_t recv_buf[sizeof(FakePandarPacket)];

    while (capturing_.load(std::memory_order_relaxed)) {
        LidarFrame frame{};

        // 流程 4：从本地 UDP 端口收包。
        const ssize_t recv_len =
            ::recvfrom(socket_fd_, recv_buf, sizeof(recv_buf), 0, nullptr, nullptr);
        if (recv_len < 0) {
            if (!capturing_.load(std::memory_order_relaxed))
                break;
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                continue;
            std::cerr << "[Pandar128] recvfrom failed errno=" << errno << '\n';
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }

        if (!decodePacket(recv_buf, static_cast<std::size_t>(recv_len), frame)) {
            std::cerr << "[Pandar128] invalid fake udp packet len=" << recv_len << '\n';
            continue;
        }

        // 流程 5：把解析后的 LidarFrame 发布到 DCMS。
        int rc = publish(&frame, sizeof(frame),
                         dcms::DataType::kLidarFrame, frame.timestamp_us);
        if (rc == 0) {
            ++frame_seq_;
        } else {
            std::cerr << "[Pandar128] publish failed rc=" << rc
                      << " frame=" << frame.frame_id << '\n';
        }
    }
}

} // namespace hal
