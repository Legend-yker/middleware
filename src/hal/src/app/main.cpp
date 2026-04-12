#include "factory/sensor_factory.h"
#include "manager/sensor_manager.h"
#include "sensor/lidar/common/lidar_base.h"
#include "sensor/lidar/common/lidar_frame.h"
#include "api/dcms.h"
#include "common/crc32.h"
#include "common/dcms_types.h"
#include "dlog/dlog.h"

#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <vector>
#include <unistd.h>

#ifndef HAL_DEFAULT_CONFIG_PATH
#define HAL_DEFAULT_CONFIG_PATH ""
#endif

namespace {
std::atomic<bool> g_running{true};

void onSignal(int /*sig*/) { g_running = false; }

void enableBrokerDemoByDefault() {
    if (std::getenv("DCMS_USE_BROKER") == nullptr)
        ::setenv("DCMS_USE_BROKER", "1", 0);
    if (std::getenv("DCMS_BROKER_SOCKET") == nullptr)
        ::setenv("DCMS_BROKER_SOCKET", "/tmp/dcms_main.sock", 0);
}

struct DemoOptions {
    bool enabled{false};
    int duration_seconds{20};
    int hz{2};
};

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

constexpr std::uint32_t kFakePandarMagic = 0x50414E44u;
constexpr std::uint16_t kFakePandarVersion = 1u;

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

void sendFakePackets(const std::string &sensor_id, std::uint16_t port,
                     const DemoOptions &options, std::atomic<int> &pending_count) {
    const int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        pending_count.fetch_sub(1, std::memory_order_relaxed);
        return;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    const int total_packets = options.duration_seconds * options.hz;
    const auto interval = std::chrono::milliseconds(1000 / options.hz);

    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    for (int i = 0; g_running.load(std::memory_order_relaxed) && i < total_packets; ++i) {
        const auto packet = buildPacket(1000u + static_cast<std::uint32_t>(i));
        const ssize_t sent = ::sendto(fd, &packet, sizeof(packet), 0,
                                      reinterpret_cast<const sockaddr *>(&addr),
                                      sizeof(addr));
        if (sent != static_cast<ssize_t>(sizeof(packet))) {
            DLOG_WARN("[MOD:hal_app]",
                      "fake send failed sensor_id=" + sensor_id +
                      " port=" + std::to_string(port));
            break;
        }

        DLOG_INFO("[MOD:hal_app]",
                  "fake send sensor_id=" + sensor_id +
                  " frame_id=" + std::to_string(packet.frame_id) +
                  " port=" + std::to_string(port) +
                  " sent=" + std::to_string(i + 1) + "/" + std::to_string(total_packets));

        std::this_thread::sleep_for(interval);
    }

    ::close(fd);
    pending_count.fetch_sub(1, std::memory_order_relaxed);
}

bool parseArgs(int argc, char *argv[], std::string &config_path, DemoOptions &options) {
    config_path = HAL_DEFAULT_CONFIG_PATH;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--demo-fake-lidar") {
            options.enabled = true;
            continue;
        }
        if (arg.rfind("--demo-seconds=", 0) == 0) {
            options.duration_seconds = std::atoi(arg.substr(15).c_str());
            continue;
        }
        if (arg.rfind("--demo-hz=", 0) == 0) {
            options.hz = std::atoi(arg.substr(10).c_str());
            continue;
        }
        config_path = arg;
    }

    if (options.duration_seconds <= 0)
        options.duration_seconds = 20;
    if (options.hz <= 0)
        options.hz = 2;
    return !config_path.empty();
}
} // namespace

int main(int argc, char *argv[]) {
    (void)hal::SensorFactory::instance();

    // 日志系统最先初始化，写入 /tmp/hal_logs，每 10s 刷写一次
    dlog::init("/tmp/hal_logs", 10, /*console_echo=*/true);
    DLOG_INFO("[MOD:hal_app]", "hal_app starting");

    std::string config_path;
    DemoOptions demo_options;
    if (!parseArgs(argc, argv, config_path, demo_options)) {
        std::cerr << "usage: hal_app [config.json] [--demo-fake-lidar] "
                  << "[--demo-seconds=20] [--demo-hz=2]\n";
        dlog::shutdown();
        return EXIT_FAILURE;
    }

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    enableBrokerDemoByDefault();
    int dcms_rc = dcms::init();
    if (dcms_rc != dcms::kDcmsOk) {
        DLOG_ERROR("[MOD:hal_app]",
                   "dcms init failed: " + std::string(dcms::dcmsErrorToString(dcms_rc)));
        dlog::shutdown();
        return EXIT_FAILURE;
    }
    DLOG_INFO("[MOD:hal_app]", "dcms initialized");

    auto &mgr = hal::SensorManager::instance();
    std::string err;

    if (!mgr.loadFromJson(config_path, err)) {
        DLOG_ERROR("[MOD:hal_app]", "load config failed: " + err);
        dcms::shutdown();
        dlog::shutdown();
        return EXIT_FAILURE;
    }

    DLOG_INFO("[MOD:hal_app]",
              "config: version=" + mgr.halJson().version() +
              " mode=" + mgr.halJson().mode() +
              " project=" + mgr.halJson().project());

    DLOG_INFO("[MOD:hal_app]",
              "sensors loaded:"
              " cameras=" + std::to_string(mgr.config().cameras().size()) +
              " lidars=" + std::to_string(mgr.config().lidars().size()) +
              " total=" + std::to_string(mgr.config().size()));

    for (auto &lidar_ptr : mgr.config().lidars()) {
        std::uint32_t topic = lidar_ptr->topicId();
        char hex[16];
        std::snprintf(hex, sizeof(hex), "%08X", topic);
        DLOG_INFO("[MOD:hal_app]",
                  "lidar ready id=" + lidar_ptr->id() +
                  " topic_id=0x" + hex);
    }

    if (!mgr.startAll(err)) {
        DLOG_ERROR("[MOD:hal_app]", "start sensors failed: " + err);
        dcms::shutdown();
        dlog::shutdown();
        return EXIT_FAILURE;
    }

    std::atomic<int> demo_pending{0};
    std::vector<std::thread> demo_threads;
    if (demo_options.enabled) {
        demo_pending.store(static_cast<int>(mgr.config().lidars().size()),
                           std::memory_order_relaxed);
        for (auto &lidar_ptr : mgr.config().lidars()) {
            auto *lidar = dynamic_cast<hal::LidarBase *>(lidar_ptr.get());
            if (!lidar) {
                demo_pending.fetch_sub(1, std::memory_order_relaxed);
                continue;
            }
            demo_threads.emplace_back(sendFakePackets,
                                      lidar->id(),
                                      lidar->dataPort(),
                                      demo_options,
                                      std::ref(demo_pending));
        }
        DLOG_INFO("[MOD:hal_app]",
                  "fake lidar demo enabled:"
                  " duration=" + std::to_string(demo_options.duration_seconds) +
                  "s hz=" + std::to_string(demo_options.hz));
    }

    DLOG_INFO("[MOD:hal_app]", "sensors started");

    while (g_running.load()) {
        if (demo_options.enabled && demo_pending.load(std::memory_order_relaxed) == 0)
            g_running = false;
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    for (auto &demo_thread : demo_threads) {
        if (demo_thread.joinable())
            demo_thread.join();
    }

    mgr.stopAll();
    dcms::shutdown();

    DLOG_INFO("[MOD:hal_app]", "stopped");
    dlog::shutdown();

    return EXIT_SUCCESS;
}
