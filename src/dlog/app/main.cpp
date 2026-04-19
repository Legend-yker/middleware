#include "sensor/lidar/common/lidar_frame.h"

#include "api/dcms.h"
#include "api/node_handle.h"
#include "api/subscriber.h"
#include "common/dcms_types.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <csignal>
#include <iostream>
#include <string>
#include <thread>

namespace {
std::atomic<bool> g_running{true};
void onSignal(int) { g_running = false; }

int fail(const std::string &msg) {
    std::cerr << "[dlog_app] " << msg << '\n';
    return EXIT_FAILURE;
}
} // namespace

int main(int argc, char *argv[]) {
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    const std::string sensor_id   = argc > 1 ? argv[1] : "128";
    int               run_seconds = argc > 2 ? std::atoi(argv[2]) : 20;
    if (run_seconds <= 0) run_seconds = 20;

    const int dcms_rc = dcms::init("dlog_app");
    if (dcms_rc != dcms::kDcmsOk)
        return fail("dcms init failed rc=" + std::to_string(dcms_rc));

    std::atomic<int> received{0};
    std::cout << "[dlog_app] subscribe topic=" << sensor_id
              << " run_seconds=" << run_seconds << '\n';

    auto sub = dcms::defaultNode().subscribe<hal::LidarFrame>(
        sensor_id, [&](const hal::LidarFrame &frame) {
            const int count =
                received.fetch_add(1, std::memory_order_relaxed) + 1;
            std::cout << "[dlog_app] recv frame_id=" << frame.frame_id
                      << " pts=" << frame.num_points
                      << " total=" << count << '\n';
        });

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(run_seconds);
    while (g_running.load() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

    if (received.load(std::memory_order_relaxed) == 0) {
        dcms::shutdown();
        return fail("no lidar frame received");
    }

    std::cout << "[dlog_app] done total_recv="
              << received.load(std::memory_order_relaxed) << '\n';
    dcms::shutdown();
    return EXIT_SUCCESS;
}
