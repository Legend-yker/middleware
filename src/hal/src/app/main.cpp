#include "factory/sensor_factory.h"
#include "manager/sensor_manager.h"
#include "sensor/lidar/common/lidar_frame.h"

#include "api/dcms.h"
#include "api/subscriber.h"
#include "common/dcms_msg.h"
#include "common/dcms_types.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

#ifndef HAL_DEFAULT_CONFIG_PATH
#define HAL_DEFAULT_CONFIG_PATH ""
#endif

namespace {
std::atomic<bool> g_running{true};

void onSignal(int /*sig*/) { g_running = false; }
} // namespace

int main(int argc, char *argv[]) {
    (void)hal::SensorFactory::instance();

    std::string config_path;
    if (argc > 1)
        config_path = argv[1];
    else
        config_path = HAL_DEFAULT_CONFIG_PATH;

    if (config_path.empty()) {
        std::cerr << "usage: hal_app <path-to-config.json>\n"
                  << "or rebuild with -DHAL_DEFAULT_CONFIG_PATH=...\n";
        return EXIT_FAILURE;
    }

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    // --- Step 1: Initialize DCMS ---
    int dcms_rc = dcms::init();
    if (dcms_rc != dcms::kDcmsOk) {
        std::cerr << "[hal_app] dcms init failed: "
                  << dcms::dcmsErrorToString(dcms_rc) << '\n';
        return EXIT_FAILURE;
    }
    std::cout << "[hal_app] dcms initialized\n";

    // --- Step 2: Load HAL config and create sensor instances ---
    auto &mgr = hal::SensorManager::instance();
    std::string err;

    if (!mgr.loadFromJson(config_path, err)) {
        std::cerr << "[hal_app] load config failed: " << err << '\n';
        dcms::shutdown();
        return EXIT_FAILURE;
    }

    std::cout << "[hal_app] config: version=" << mgr.halJson().version()
              << " mode=" << mgr.halJson().mode()
              << " project=" << mgr.halJson().project() << '\n';

    std::cout << "[hal_app] sensors loaded: "
              << "cameras=" << mgr.config().cameras().size()
              << " lidars=" << mgr.config().lidars().size()
              << " total=" << mgr.config().size() << '\n';

    // --- Step 3: Subscribe to lidar topics before starting capture ---
    std::atomic<int> frame_count{0};
    std::vector<std::unique_ptr<dcms::Subscriber<hal::LidarFrame>>> lidar_subs;

    for (auto &lidar_ptr : mgr.config().lidars()) {
        std::uint32_t topic = lidar_ptr->topicId();
        std::string sid = lidar_ptr->id();
        std::cout << "[hal_app] subscribing lidar \"" << sid
                  << "\" topic_id=0x" << std::hex << topic << std::dec << '\n';

        lidar_subs.push_back(
            std::make_unique<dcms::Subscriber<hal::LidarFrame>>(
                topic,
                [&frame_count, sid](const dcms::MsgMeta &meta,
                                    const hal::LidarFrame &frame) {
                    int cnt =
                        frame_count.fetch_add(1, std::memory_order_relaxed) + 1;
                    std::cout << "[hal_app] << " << sid
                              << " frame_id=" << frame.frame_id
                              << " pts=" << frame.num_points
                              << " seq=" << meta.seq
                              << " capture_ts=" << meta.timestamp_capture
                              << " publish_ts=" << meta.timestamp_publish
                              << " (total_recv=" << cnt << ")\n";
                }));
    }

    // --- Step 4: Start all sensors (triggers Pandar128 UDP sim thread) ---
    if (!mgr.startAll(err)) {
        std::cerr << "[hal_app] start sensors failed: " << err << '\n';
        dcms::shutdown();
        return EXIT_FAILURE;
    }

    std::cout << "[hal_app] sensors started, running (SIGINT/SIGTERM to exit)\n";

    // --- Step 5: Main loop ---
    while (g_running.load())
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // --- Cleanup ---
    mgr.stopAll();
    std::cout << "[hal_app] total lidar frames received via DCMS: "
              << frame_count.load() << '\n';

    lidar_subs.clear();
    dcms::shutdown();

    std::cout << "[hal_app] stopped\n";
    return EXIT_SUCCESS;
}
