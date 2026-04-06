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
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

#ifndef HAL_TEST_CONFIG_FILE
#error HAL_TEST_CONFIG_FILE must be defined
#endif

namespace {

constexpr std::uint32_t kFakePandarMagic = 0x50414E44u;
constexpr std::uint16_t kFakePandarVersion = 1u;
constexpr std::uint32_t kBaseFrameId = 100u;
constexpr int kPacketsToSend = 3;

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

static_assert(sizeof(FakePandarPoint) == 16);

std::uint64_t nowUs() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

FakePandarPacket buildPacket(std::uint32_t frame_id) {
    FakePandarPacket packet{};
    packet.magic = kFakePandarMagic;
    packet.version = kFakePandarVersion;
    packet.point_count = hal::LidarFrame::kMaxDemoPoints;
    packet.frame_id = frame_id;
    packet.timestamp_us = nowUs();

    for (std::uint32_t i = 0; i < packet.point_count; ++i) {
        packet.points[i].x = static_cast<float>(frame_id) + static_cast<float>(i) * 0.25f;
        packet.points[i].y = static_cast<float>(i) * 1.5f;
        packet.points[i].z = -static_cast<float>(i) * 0.1f;
        packet.points[i].intensity = static_cast<std::uint8_t>(20 + i);
        packet.points[i].ring = static_cast<std::uint8_t>((frame_id + i) % 128);
    }

    packet.payload_crc32 =
        dcms::crc32(&packet, offsetof(FakePandarPacket, payload_crc32));
    return packet;
}

void sendFakePackets(std::uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    for (int i = 0; i < kPacketsToSend; ++i) {
        const FakePandarPacket packet = buildPacket(kBaseFrameId + static_cast<std::uint32_t>(i));
        const ssize_t sent = ::sendto(fd, &packet, sizeof(packet), 0,
                                      reinterpret_cast<const sockaddr *>(&addr),
                                      sizeof(addr));
        if (sent != static_cast<ssize_t>(sizeof(packet)))
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
    }

    ::close(fd);
}

} // namespace

int main() {
    auto fail = [](const std::string &msg) {
        std::cerr << "hal_sensor_smoke: " << msg << '\n';
        return EXIT_FAILURE;
    };

    (void)hal::SensorFactory::instance();

    int dcms_rc = dcms::init();
    if (dcms_rc != dcms::kDcmsOk)
        return fail("dcms init failed rc=" + std::to_string(dcms_rc));

    auto &mgr = hal::SensorManager::instance();
    mgr.clear();

    std::string err;
    if (!mgr.loadFromJson(HAL_TEST_CONFIG_FILE, err)) {
        dcms::shutdown();
        return fail("loadFromJson failed: " + err);
    }

    hal::Config &cfg = mgr.config();
    if (cfg.size() != 2 || cfg.cameras().size() != 1 || cfg.lidars().size() != 1) {
        mgr.clear();
        dcms::shutdown();
        return fail("unexpected config layout");
    }

    const auto *pandar = dynamic_cast<const hal::Pandar128Driver *>(
        cfg.lidars()[0].get());
    if (pandar == nullptr) {
        mgr.clear();
        dcms::shutdown();
        return fail("pandar driver cast failed");
    }
    if (pandar->ip() != "192.168.1.201" || pandar->dataPort() != 2368 || cfg.get("128") == nullptr) {
        mgr.clear();
        dcms::shutdown();
        return fail("pandar config mismatch");
    }

    if (mgr.halJson().version() != "v1" || mgr.halJson().mode() != "online") {
        mgr.clear();
        dcms::shutdown();
        return fail("hal json header mismatch");
    }

    std::atomic<int> received{0};
    std::atomic<bool> callback_failed{false};
    ReceivedState state{};
    std::uint32_t lidar_topic = cfg.lidars()[0]->topicId();
    if (lidar_topic == 0) {
        mgr.clear();
        dcms::shutdown();
        return fail("invalid lidar topic");
    }

    {
        dcms::Subscriber<hal::LidarFrame> sub(
            lidar_topic, [&](const dcms::MsgMeta &meta, const hal::LidarFrame &frame) {
                const bool valid =
                    frame.num_points == hal::LidarFrame::kMaxDemoPoints &&
                    meta.module_id == static_cast<std::uint32_t>(dcms::ModuleId::kHal) &&
                    meta.data_type == static_cast<std::uint32_t>(dcms::DataType::kLidarFrame) &&
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
            });

        if (!mgr.startAll(err)) {
            mgr.clear();
            dcms::shutdown();
            return fail("startAll failed: " + err);
        }

        std::thread sender([&] { sendFakePackets(pandar->dataPort()); });

        for (int i = 0; i < 200 && received.load(std::memory_order_relaxed) < kPacketsToSend; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));

        sender.join();
        mgr.stopAll();
    }

    if (callback_failed.load(std::memory_order_relaxed) ||
        received.load(std::memory_order_relaxed) != kPacketsToSend) {
        mgr.clear();
        dcms::shutdown();
        return fail("did not receive expected fake UDP frames");
    }

    dcms::MsgMeta last_meta{};
    hal::LidarFrame last_frame{};
    {
        std::lock_guard<std::mutex> lk(state.mutex);
        last_meta = state.meta;
        last_frame = state.frame;
        if (state.count != kPacketsToSend) {
            mgr.clear();
            dcms::shutdown();
            return fail("state count mismatch");
        }
    }

    if (last_meta.seq != static_cast<std::uint64_t>(kPacketsToSend - 1) ||
        last_meta.payload_len != sizeof(hal::LidarFrame) ||
        last_frame.frame_id != kBaseFrameId + static_cast<std::uint32_t>(kPacketsToSend - 1) ||
        last_frame.points[0].x != static_cast<float>(last_frame.frame_id) ||
        last_frame.points[1].y != 1.5f ||
        last_frame.points[2].z != -0.2f ||
        last_frame.points[3].intensity != 23 ||
        last_frame.points[4].ring !=
            static_cast<std::uint8_t>((last_frame.frame_id + 4u) % 128u)) {
        mgr.clear();
        dcms::shutdown();
        return fail("decoded frame content mismatch");
    }

    mgr.clear();
    dcms::shutdown();

    std::cout << "hal_sensor_smoke: ok (received " << received.load(std::memory_order_relaxed)
              << " lidar frames via fake UDP -> HAL -> DCMS, last_seq=" << last_meta.seq
              << ")\n";
    return EXIT_SUCCESS;
}
