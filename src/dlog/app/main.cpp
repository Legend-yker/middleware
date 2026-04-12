#include "sensor/lidar/common/lidar_frame.h"

#include "api/dcms.h"
#include "api/subscriber.h"
#include "common/dcms_msg.h"
#include "common/dcms_types.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <csignal>
#include <functional>
#include <iostream>
#include <string>
#include <thread>

namespace {
std::atomic<bool> g_running{true};

void onSignal(int) { g_running = false; }

void enableBrokerDemoByDefault() {
    if (std::getenv("DCMS_USE_BROKER") == nullptr)
        ::setenv("DCMS_USE_BROKER", "1", 0);
    if (std::getenv("DCMS_BROKER_SOCKET") == nullptr)
        ::setenv("DCMS_BROKER_SOCKET", "/tmp/dcms_main.sock", 0);
}

int fail(const std::string &msg) {
    std::cerr << "[dlog_app] " << msg << '\n';
    return EXIT_FAILURE;
}

} // namespace

int main(int argc, char *argv[]) {
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    enableBrokerDemoByDefault();

    const std::string sensor_id = argc > 1 ? argv[1] : "128";
    int run_seconds = argc > 2 ? std::atoi(argv[2]) : 20;
    if (run_seconds <= 0)
        run_seconds = 20;
    const std::uint32_t lidar_topic =
        static_cast<std::uint32_t>(std::hash<std::string>{}(sensor_id));

    const int dcms_rc = dcms::init();
    if (dcms_rc != dcms::kDcmsOk)
        return fail("dcms init failed rc=" + std::to_string(dcms_rc));

    std::atomic<int> received{0};
    std::atomic<bool> callback_failed{false};
    std::cout << "[dlog_app] subscribe topic_id=0x" << std::hex << lidar_topic
              << std::dec << " sensor_id=" << sensor_id
              << " run_seconds=" << run_seconds << '\n';

    dcms::Subscriber<hal::LidarFrame> sub(
        lidar_topic, [&](const dcms::MsgMeta &meta, const hal::LidarFrame &frame) {
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

            const int count = received.fetch_add(1, std::memory_order_relaxed) + 1;
            std::cout << "[dlog_app] recv frame_id=" << frame.frame_id
                      << " pts=" << frame.num_points
                      << " seq=" << meta.seq
                      << " capture_ts=" << meta.timestamp_capture
                      << " publish_ts=" << meta.timestamp_publish
                      << " total=" << count << '\n';
        });

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(run_seconds);
    while (g_running.load() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

    if (callback_failed.load(std::memory_order_relaxed) ||
        received.load(std::memory_order_relaxed) == 0) {
        dcms::shutdown();
        return fail("no valid lidar frame received");
    }

    std::cout << "[dlog_app] done total_recv=" << received.load(std::memory_order_relaxed)
              << '\n';
    dcms::shutdown();
    return EXIT_SUCCESS;
}
