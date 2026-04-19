// dcms_smoke: end-to-end pub/sub test using the NodeHandle API.
//
// Requires a running dcms_daemon.  Launch before running:
//   ./dcms_daemon &
//
// Four-phase flow:
//   Phase 1 — NodeHandle::create() connects to Daemon
//   Phase 2 — advertise() registers topic, Daemon creates shm ring buffer
//   Phase 3 — subscribe() maps same ring buffer, receives eventfd
//   Phase 4 — publish() writes directly to ring, subscriber wakes via eventfd
//
// Logs written to /tmp/dcms_logs/ (queryable with zcat + grep MOD:dcms_smoke)

#include "api/dcms.h"
#include "api/node_handle.h"
#include "api/publisher.h"
#include "api/subscriber.h"
#include "common/dcms_msg.h"

#include "dlog/dlog.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <thread>

#define LOG(msg) DLOG_INFO("[MOD:dcms_smoke]", msg)

struct SimplePayload {
    std::uint32_t id;
    float         value;
};

int main() {
    dlog::init("/tmp/dcms_logs", 5, /*console_echo=*/true);
    LOG("dcms_smoke start");

    // ── Phase 1: connect to Daemon ──────────────────────────────────────────
    dcms::NodeHandle pub_node, sub_node;

    int rc = dcms::NodeHandle::create("smoke_pub", pub_node);
    if (rc != dcms::kDcmsOk) {
        std::cerr << "NodeHandle::create(smoke_pub) failed: " << rc << '\n'
                  << "Is dcms_daemon running?\n";
        dlog::shutdown();
        return EXIT_FAILURE;
    }
    assert(pub_node.valid());
    LOG("phase1: smoke_pub connected to daemon");

    rc = dcms::NodeHandle::create("smoke_sub", sub_node);
    assert(rc == dcms::kDcmsOk);
    assert(sub_node.valid());
    LOG("phase1: smoke_sub connected to daemon");

    // ── Phase 2: advertise ──────────────────────────────────────────────────
    auto pub = pub_node.advertise<SimplePayload>("smoke/test", 8);
    assert(pub.valid());
    LOG("phase2: advertised topic smoke/test (8 slots, slot_size=" +
        std::to_string(sizeof(SimplePayload)) + ")");

    // ── Phase 3: subscribe ──────────────────────────────────────────────────
    std::atomic<int> received{0};
    SimplePayload    last{};

    auto sub = sub_node.subscribe<SimplePayload>(
        "smoke/test",
        [&](const SimplePayload &m) {
            last = m;
            int n = received.fetch_add(1, std::memory_order_relaxed) + 1;
            LOG("sub_recv: id=" + std::to_string(m.id) +
                " value=" + std::to_string(m.value) +
                " total=" + std::to_string(n));
        });
    assert(sub.valid());
    LOG("phase3: subscribed to smoke/test");

    // Give Daemon time to deliver kNewSubscriberNotify to publisher
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // ── Phase 4: publish ────────────────────────────────────────────────────
    SimplePayload msg{42, 3.14f};
    rc = pub.publish(msg);
    assert(rc == dcms::kDcmsOk);
    LOG("phase4: published id=42 value=3.14");

    // Wait up to 500ms for subscriber callback
    for (int i = 0; i < 50 && received.load() == 0; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

    assert(received.load() >= 1);
    assert(last.id == 42);
    LOG("phase4: first message received OK");

    // Publish multiple messages
    for (int i = 1; i <= 5; ++i) {
        SimplePayload m{static_cast<std::uint32_t>(i), static_cast<float>(i)};
        assert(pub.publish(m) == dcms::kDcmsOk);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    assert(received.load() >= 6);
    LOG("phase4: 5 additional messages received, total=" +
        std::to_string(received.load()));

    LOG("dcms_smoke: all assertions passed");
    std::cout << "dcms_smoke: ok\n";

    dlog::shutdown();
    return EXIT_SUCCESS;
}
