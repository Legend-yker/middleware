#include "factory/sensor_factory.h"
#include "manager/sensor_manager.h"
#include "sensor/lidar/common/lidar_frame.h"
#include "sensor/lidar/lidar_pandar128/pandar128_driver.h"

#include "api/dcms.h"
#include "api/subscriber.h"
#include "common/crc32.h"
#include "common/dcms_msg.h"
#include "common/dcms_types.h"

#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

#ifndef DLOG_DEFAULT_HAL_CONFIG_PATH
#define DLOG_DEFAULT_HAL_CONFIG_PATH ""
#endif

namespace {

// dlog 演示程序使用的 fake Pandar 协议。
// 目的不是模拟完整真机协议，而是把“发包 -> HAL 收包 -> DCMS 分发 -> dlog 订阅”整条链路先跑通。
constexpr std::uint32_t kFakePandarMagic = 0x50414E44u;
constexpr std::uint16_t kFakePandarVersion = 1u;
constexpr std::uint32_t kBaseFrameId = 1000u;
constexpr int kPacketsToSend = 5;

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

struct ReceivedState {
    std::mutex mutex;
    dcms::MsgMeta meta{};
    hal::LidarFrame frame{};
    int count{0};
};

std::uint64_t nowUs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

FakePandarPacket buildPacket(std::uint32_t frame_id) {
    // 模块 A：构造 fake UDP 点云包。
    FakePandarPacket packet{};
    packet.magic = kFakePandarMagic;
    packet.version = kFakePandarVersion;
    packet.point_count = hal::LidarFrame::kMaxDemoPoints;
    packet.frame_id = frame_id;
    packet.timestamp_us = nowUs();

    for (std::uint32_t i = 0; i < packet.point_count; ++i) {
        packet.points[i].x = static_cast<float>(frame_id) + static_cast<float>(i) * 0.5f;
        packet.points[i].y = static_cast<float>(i) * 2.0f;
        packet.points[i].z = static_cast<float>(i) * 0.2f;
        packet.points[i].intensity = static_cast<std::uint8_t>(30 + i);
        packet.points[i].ring = static_cast<std::uint8_t>((frame_id + i) % 128u);
    }

    packet.payload_crc32 =
        dcms::crc32(&packet, offsetof(FakePandarPacket, payload_crc32));
    return packet;
}

void sendFakePackets(std::uint16_t port) {
    // 模块 B：通过本地 UDP 端口把 fake 包发给 HAL 的 Pandar128Driver。
    const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    for (int i = 0; i < kPacketsToSend; ++i) {
        const FakePandarPacket packet =
            buildPacket(kBaseFrameId + static_cast<std::uint32_t>(i));
        const ssize_t sent = ::sendto(fd, &packet, sizeof(packet), 0,
                                      reinterpret_cast<const sockaddr *>(&addr),
                                      sizeof(addr));
        if (sent != static_cast<ssize_t>(sizeof(packet)))
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    ::close(fd);
}

int fail(const std::string &msg) {
    std::cerr << "[dlog_demo] " << msg << '\n';
    return EXIT_FAILURE;
}

} // namespace

int main(int argc, char *argv[]) {
    (void)hal::SensorFactory::instance();

    // 步骤 1：读取 HAL 配置文件路径。
    std::string config_path = argc > 1 ? argv[1] : DLOG_DEFAULT_HAL_CONFIG_PATH;
    if (config_path.empty())
        return fail("missing hal config path");

    // 步骤 2：初始化 DCMS。
    const int dcms_rc = dcms::init();
    if (dcms_rc != dcms::kDcmsOk)
        return fail("dcms init failed rc=" + std::to_string(dcms_rc));

    // 步骤 3：加载 HAL 配置，并拿到 lidar 实例。
    auto &mgr = hal::SensorManager::instance();
    mgr.clear();

    std::string err;
    if (!mgr.loadFromJson(config_path, err)) {
        dcms::shutdown();
        return fail("loadFromJson failed: " + err);
    }

    if (mgr.config().lidars().empty()) {
        mgr.clear();
        dcms::shutdown();
        return fail("no lidar configured");
    }

    auto *lidar_base = mgr.config().lidars()[0].get();
    const auto *pandar = dynamic_cast<const hal::Pandar128Driver *>(lidar_base);
    if (pandar == nullptr) {
        mgr.clear();
        dcms::shutdown();
        return fail("first lidar is not Pandar128");
    }

    const std::uint32_t lidar_topic = pandar->topicId();
    std::cout << "[dlog_demo] subscribe topic_id=0x" << std::hex << lidar_topic
              << std::dec << " sensor_id=" << pandar->id()
              << " port=" << pandar->dataPort() << '\n';

    // 步骤 4：在 dlog 模块侧创建订阅者。
    std::atomic<int> received{0};
    std::atomic<bool> callback_failed{false};
    ReceivedState state{};

    {
        dcms::Subscriber<hal::LidarFrame> sub(
            lidar_topic, [&](const dcms::MsgMeta &meta, const hal::LidarFrame &frame) {
                // 模块 C：回调里检查 meta 和 payload 是否合理。
                const bool valid =
                    frame.num_points == hal::LidarFrame::kMaxDemoPoints &&
                    meta.module_id == static_cast<std::uint32_t>(dcms::ModuleId::kHal) &&
                    meta.data_type ==
                        static_cast<std::uint32_t>(dcms::DataType::kLidarFrame) &&
                    meta.timestamp_capture > 0 &&
                    meta.payload_crc32 != 0;
                if (!valid) {
                    callback_failed.store(true, std::memory_order_relaxed);
                    return;
                }

                std::lock_guard<std::mutex> lk(state.mutex);
                state.meta = meta;
                state.frame = frame;
                state.count += 1;
                received.store(state.count, std::memory_order_relaxed);
                std::cout << "[dlog_demo] recv frame_id=" << frame.frame_id
                          << " pts=" << frame.num_points
                          << " seq=" << meta.seq
                          << " capture_ts=" << meta.timestamp_capture
                          << " publish_ts=" << meta.timestamp_publish
                          << " total=" << state.count << '\n';
            });

        if (!mgr.startAll(err)) {
            mgr.clear();
            dcms::shutdown();
            return fail("startAll failed: " + err);
        }

        // 步骤 5：启动 fake sender，给 HAL 的 UDP 端口送测试数据。
        std::thread sender([&] { sendFakePackets(pandar->dataPort()); });

        // 步骤 6：等待订阅侧收到足够的帧。
        for (int i = 0;
             i < 300 && received.load(std::memory_order_relaxed) < kPacketsToSend;
             ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        sender.join();
        mgr.stopAll();
    }

    if (callback_failed.load(std::memory_order_relaxed) ||
        received.load(std::memory_order_relaxed) != kPacketsToSend) {
        mgr.clear();
        dcms::shutdown();
        return fail("did not receive all fake lidar frames");
    }

    // 步骤 7：输出最终结果，作为端到端成功标志。
    dcms::MsgMeta last_meta{};
    hal::LidarFrame last_frame{};
    {
        std::lock_guard<std::mutex> lk(state.mutex);
        last_meta = state.meta;
        last_frame = state.frame;
    }

    std::cout << "[dlog_demo] ok last_frame_id=" << last_frame.frame_id
              << " last_seq=" << last_meta.seq
              << " point0=(" << last_frame.points[0].x << ", "
              << last_frame.points[0].y << ", " << last_frame.points[0].z
              << ")\n";

    mgr.clear();
    dcms::shutdown();
    return EXIT_SUCCESS;
}
