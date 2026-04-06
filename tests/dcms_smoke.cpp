#include "api/dcms_api.h"
#include "common/crc32.h"
#include "common/dcms_msg.h"
#include "common/dcms_types.h"
#include "api/dcms.h"
#include "api/publisher.h"
#include "api/subscriber.h"
#include "utils/msg_queue.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <thread>

struct SimplePayload {
    std::uint32_t id;
    float value;
};

int main() {
    // --- CRC32 ---
    const char *payload = "hello dcms";
    std::uint32_t c = dcms::crc32(payload, std::strlen(payload));
    assert(c != 0);
    assert(dcms::crc32(payload, std::strlen(payload)) == c);

    // --- MsgQueue ---
    dcms::MsgQueue q(4);
    assert(q.empty());
    int dummy1 = 1, dummy2 = 2, dummy3 = 3, dummy4 = 4, dummy5 = 5;
    assert(q.push(&dummy1));
    assert(q.push(&dummy2));
    assert(q.push(&dummy3));
    assert(q.push(&dummy4));
    assert(!q.push(&dummy5));
    assert(q.full());
    void *out = nullptr;
    assert(q.pop(out));
    assert(out == &dummy1);
    assert(q.pop(out));
    assert(out == &dummy2);

    // --- MsgMeta size ---
    assert(sizeof(dcms::MsgMeta) == 40);

    // --- dcms::init / publish / subscribe / shutdown ---
    int rc = dcms::init("");
    assert(rc == dcms::kDcmsOk);

    std::atomic<int> received{0};
    SimplePayload last_payload{};
    dcms::MsgMeta last_meta{};

    dcms::Subscriber<SimplePayload> sub("smoke_topic",
        [&](const dcms::MsgMeta &meta, const SimplePayload &m) {
            last_meta = meta;
            last_payload = m;
            received.fetch_add(1, std::memory_order_relaxed);
        });

    dcms::Publisher pub("smoke_topic");

    // Build Msg<SimplePayload> with metadata
    SimplePayload out_data{42, 3.14f};
    dcms::Msg<SimplePayload> msg{};
    msg.meta.module_id = static_cast<std::uint32_t>(dcms::ModuleId::kDcms);
    msg.meta.instance_id = pub.topic();
    msg.meta.data_type = static_cast<std::uint32_t>(dcms::DataType::kRaw);
    msg.meta.seq = 0;
    msg.meta.timestamp_capture = dcms::nowUs();
    msg.meta.timestamp_publish = dcms::nowUs();
    msg.meta.payload_len = sizeof(SimplePayload);
    msg.meta.payload_crc32 = dcms::crc32(&out_data, sizeof(SimplePayload));
    msg.data = out_data;

    rc = pub.publish(msg);
    assert(rc == dcms::kDcmsOk);

    for (int i = 0; i < 50 && received.load() == 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

    assert(received.load() >= 1);
    assert(last_payload.id == 42);

    // Verify metadata was delivered
    assert(last_meta.module_id == static_cast<std::uint32_t>(dcms::ModuleId::kDcms));
    assert(last_meta.seq == 0);
    assert(last_meta.payload_len == sizeof(SimplePayload));
    assert(last_meta.timestamp_capture > 0);

    // CRC integrity: tamper payload_crc32 → Publisher should reject
    dcms::MsgMeta bad_meta = msg.meta;
    bad_meta.payload_crc32 = 0xDEADBEEF;
    rc = pub.publish(bad_meta, &out_data, sizeof(SimplePayload));
    assert(rc == dcms::kDcmsErrCrcMismatch);

    dcms::shutdown();

    // After shutdown, publish should fail
    rc = pub.publish(msg);
    assert(rc == dcms::kDcmsErrNotInit);

    std::cout << "dcms_smoke: ok\n";
    return EXIT_SUCCESS;
}
