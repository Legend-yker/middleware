#include "api/dcms.h"
#include "api/publisher.h"
#include "api/subscriber.h"
#include "common/crc32.h"
#include "common/dcms_msg.h"
#include "common/dcms_types.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <thread>

namespace {
std::atomic<bool> g_running{true};
void onSignal(int) { g_running = false; }
} // namespace

struct TestPayload {
    std::uint32_t seq;
    float value;
};

int main(int argc, char *argv[]) {
    const char *cfg = (argc > 1) ? argv[1] : "";

    int rc = dcms::init(cfg);
    if (rc != dcms::kDcmsOk) {
        std::cerr << "[dcms_app] init failed: " << dcms::dcmsErrorToString(rc)
                  << '\n';
        return EXIT_FAILURE;
    }

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    std::atomic<int> recv_count{0};

    dcms::Subscriber<TestPayload> sub(
        "test_topic",
        [&recv_count](const dcms::MsgMeta &meta, const TestPayload &msg) {
            recv_count.fetch_add(1, std::memory_order_relaxed);
            std::cout << "[dcms_app] recv seq=" << msg.seq
                      << " val=" << msg.value
                      << " meta.seq=" << meta.seq << '\n';
        });

    dcms::Publisher pub("test_topic");

    std::uint32_t seq = 0;
    std::cout << "[dcms_app] running (SIGINT to exit)\n";

    while (g_running.load()) {
        TestPayload payload{seq++, seq * 0.1f};

        dcms::Msg<TestPayload> msg{};
        msg.meta.module_id =
            static_cast<std::uint32_t>(dcms::ModuleId::kDcms);
        msg.meta.instance_id = pub.topic();
        msg.meta.data_type = static_cast<std::uint32_t>(dcms::DataType::kRaw);
        msg.meta.seq = payload.seq;
        msg.meta.timestamp_capture = dcms::nowUs();
        msg.meta.timestamp_publish = dcms::nowUs();
        msg.meta.payload_len = sizeof(TestPayload);
        msg.meta.payload_crc32 =
            dcms::crc32(&payload, sizeof(TestPayload));
        msg.data = payload;

        int r = pub.publish(msg);
        if (r != dcms::kDcmsOk)
            std::cerr << "[dcms_app] publish err: "
                      << dcms::dcmsErrorToString(r) << '\n';
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    std::cout << "[dcms_app] total recv=" << recv_count.load() << '\n';
    dcms::shutdown();
    std::cout << "[dcms_app] stopped\n";
    return EXIT_SUCCESS;
}
