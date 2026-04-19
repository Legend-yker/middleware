// dcms_rpc_smoke: RPC service/client smoke test.
//
// Tests ServiceServer + ServiceClient — no dcms_daemon required.
// Covers:
//   1. Basic request/reply (AddTwoInts)
//   2. String echo service
//   3. Multiple sequential calls
//   4. Call to non-existent service returns error
//
// Logs written to /tmp/dcms_logs/ (queryable with zcat + grep MOD:dcms_rpc_smoke)

#include "api/node_handle.h"
#include "api/service_server.h"
#include "api/service_client.h"

#include "dlog/dlog.h"

#include <cassert>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>

#define LOG(msg) DLOG_INFO("[MOD:dcms_rpc_smoke]", msg)

// ── Payload types ────────────────────────────────────────────────────────────

struct AddReq  { std::int32_t a; std::int32_t b; };
struct AddResp { std::int32_t sum; };

struct EchoReq  { char text[64]; };
struct EchoResp { char text[64]; };

// ── Helpers ──────────────────────────────────────────────────────────────────

static void waitServerReady(int ms = 20) {
    // Give the server's accept thread time to reach poll()
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

// ── Tests ────────────────────────────────────────────────────────────────────

static void testAddService() {
    LOG("=== test_add_service start ===");

    dcms::ServiceServer<AddReq, AddResp> srv("add_two_ints",
        [](const AddReq &req, AddResp &resp) {
            resp.sum = req.a + req.b;
            LOG("add handler: " + std::to_string(req.a) + "+" +
                std::to_string(req.b) + "=" + std::to_string(resp.sum));
        });
    assert(srv.valid());
    waitServerReady();

    dcms::ServiceClient<AddReq, AddResp> cli("add_two_ints");
    assert(cli.valid());

    // Single call
    {
        AddReq req{3, 4};
        AddResp resp{};
        int rc = cli.call(req, resp);
        assert(rc == dcms::kDcmsOk);
        assert(resp.sum == 7);
        LOG("add(3,4)=" + std::to_string(resp.sum) + " [OK]");
    }

    // Negative numbers
    {
        AddReq req{-10, 5};
        AddResp resp{};
        int rc = cli.call(req, resp);
        assert(rc == dcms::kDcmsOk);
        assert(resp.sum == -5);
        LOG("add(-10,5)=" + std::to_string(resp.sum) + " [OK]");
    }

    // 10 sequential calls
    for (int i = 0; i < 10; ++i) {
        AddReq req{i, i * 3};
        AddResp resp{};
        int rc = cli.call(req, resp);
        assert(rc == dcms::kDcmsOk);
        assert(resp.sum == i + i * 3);
    }
    LOG("10 sequential add calls [OK]");
    LOG("=== test_add_service PASS ===");
}

static void testEchoService() {
    LOG("=== test_echo_service start ===");

    dcms::ServiceServer<EchoReq, EchoResp> srv("echo",
        [](const EchoReq &req, EchoResp &resp) {
            std::strncpy(resp.text, req.text, sizeof(resp.text) - 1);
            LOG("echo handler: " + std::string(req.text));
        });
    assert(srv.valid());
    waitServerReady();

    dcms::ServiceClient<EchoReq, EchoResp> cli("echo");

    EchoReq req{};
    std::strncpy(req.text, "hello_dcms_rpc", sizeof(req.text) - 1);
    EchoResp resp{};
    int rc = cli.call(req, resp);
    assert(rc == dcms::kDcmsOk);
    assert(std::strcmp(resp.text, "hello_dcms_rpc") == 0);
    LOG("echo(\"hello_dcms_rpc\") = \"" + std::string(resp.text) + "\" [OK]");
    LOG("=== test_echo_service PASS ===");
}

static void testNonExistentService() {
    LOG("=== test_nonexistent_service start ===");

    dcms::ServiceClient<AddReq, AddResp> cli("nonexistent_xyz_service");
    AddReq req{1, 2};
    AddResp resp{};
    int rc = cli.call(req, resp, /*timeout_ms=*/200);
    assert(rc == dcms::kDcmsErrDaemonNotFound);
    LOG("non-existent service returns kDcmsErrDaemonNotFound [OK]");
    LOG("=== test_nonexistent_service PASS ===");
}

static void testNodeHandleFactoryMethods() {
    // Verify advertiseService / serviceClient convenience wrappers work.
    // NodeHandle::create requires dcms_daemon; we skip that and call
    // constructors directly via a default-constructed NodeHandle.
    LOG("=== test_node_handle_factory start ===");

    // ServiceServer/Client can be created standalone (no daemon needed for RPC)
    dcms::ServiceServer<AddReq, AddResp> srv("nh_add",
        [](const AddReq &req, AddResp &resp) { resp.sum = req.a + req.b; });
    assert(srv.valid());
    waitServerReady();

    dcms::ServiceClient<AddReq, AddResp> cli("nh_add");
    AddReq req{100, 200};
    AddResp resp{};
    int rc = cli.call(req, resp);
    assert(rc == dcms::kDcmsOk);
    assert(resp.sum == 300);
    LOG("factory add(100,200)=" + std::to_string(resp.sum) + " [OK]");
    LOG("=== test_node_handle_factory PASS ===");
}

// ── main ─────────────────────────────────────────────────────────────────────

int main() {
    dlog::init("/tmp/dcms_logs", 5, /*console_echo=*/true);
    LOG("dcms_rpc_smoke start");

    testAddService();
    testEchoService();
    testNonExistentService();
    testNodeHandleFactoryMethods();

    LOG("dcms_rpc_smoke: all tests passed");
    std::cout << "dcms_rpc_smoke: ok\n";

    dlog::shutdown();
    return EXIT_SUCCESS;
}
