#include "api/dcms_api.h"

#include "common/crc32.h"
#include "common/dcms_types.h"
#include "manager/recv_thread.h"
#include "shm/shm_pool.h"
#include "transport/common/ipc_backend.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <mutex>

namespace {

struct DcmsContext {
    std::shared_ptr<dcms::IpcBackend> ipc;
    std::unique_ptr<dcms::RecvThread> recv_thread;
    std::atomic<std::uint32_t> seq_counter{0};
    bool inited{false};
};

std::mutex g_ctx_mutex;
DcmsContext g_ctx;

std::uint64_t nowUs() {
    using namespace std::chrono;
    return static_cast<std::uint64_t>(
        duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count());
}

} // namespace

extern "C" {

int dcms_init(const char *config_path) {
    (void)config_path;
    std::lock_guard<std::mutex> lk(g_ctx_mutex);
    if (g_ctx.inited)
        return dcms::kDcmsOk;

    if (!dcms::ShmPool::instance().create("dcms_shm", 4 * 1024 * 1024))
        return dcms::kDcmsErrShmFail;

    g_ctx.ipc = std::shared_ptr<dcms::IpcBackend>(dcms::createIpcBackend());
    if (!g_ctx.ipc || !g_ctx.ipc->init("dcms_main"))
        return dcms::kDcmsErrNotImpl;

    g_ctx.recv_thread = std::make_unique<dcms::RecvThread>(g_ctx.ipc);
    g_ctx.recv_thread->start();

    g_ctx.inited = true;
    return dcms::kDcmsOk;
}

void dcms_shutdown(void) {
    std::lock_guard<std::mutex> lk(g_ctx_mutex);
    if (!g_ctx.inited)
        return;
    if (g_ctx.recv_thread)
        g_ctx.recv_thread->stop();
    g_ctx.recv_thread.reset();
    if (g_ctx.ipc)
        g_ctx.ipc->shutdown();
    g_ctx.ipc.reset();
    dcms::ShmPool::instance().destroy();
    g_ctx.inited = false;
}

int dcms_msg_publish(dcms_topic_id topic, const void *data, std::uint32_t len) {
    if (!g_ctx.inited)
        return dcms::kDcmsErrNotInit;

    const std::size_t total_size = sizeof(dcms::DcmsMsgHeader) + len;
    void *buf = dcms::ShmPool::instance().alloc(total_size);
    if (!buf)
        return dcms::kDcmsErrShmFail;

    auto *hdr = static_cast<dcms::DcmsMsgHeader *>(buf);
    std::memset(hdr, 0, sizeof(dcms::DcmsMsgHeader));
    hdr->topic_id = topic;
    hdr->seq = g_ctx.seq_counter.fetch_add(1, std::memory_order_relaxed);
    hdr->msg_type = dcms::MsgType::kPublish;
    hdr->data_offset = static_cast<std::uint32_t>(sizeof(dcms::DcmsMsgHeader));
    hdr->data_len = len;

    if (data && len > 0)
        std::memcpy(dcms::msgDataPtr(hdr), data, len);

    const auto ts = nowUs();
    hdr->timestamp_publish = ts;
    hdr->timestamp_system = ts;
    hdr->timestamp_transfer = ts;

    hdr->crc32 = 0;
    hdr->crc32 = dcms::crc32(hdr, total_size);

    if (!g_ctx.ipc->sendNotify(buf, total_size)) {
        dcms::ShmPool::instance().free(buf);
        return dcms::kDcmsErrBusy;
    }

    return dcms::kDcmsOk;
}

int dcms_msg_subscribe(dcms_topic_id topic, dcms_msg_callback cb, void *user) {
    if (!g_ctx.inited)
        return dcms::kDcmsErrNotInit;
    if (!cb)
        return dcms::kDcmsErrInvalidArg;

    g_ctx.recv_thread->subscribe(topic,
        reinterpret_cast<std::uintptr_t>(cb),
        user,
        [cb, user](const dcms::DcmsMsgHeader *hdr, const void *data, std::size_t len) {
            cb(hdr->topic_id, data, static_cast<std::uint32_t>(len), user);
        });

    return dcms::kDcmsOk;
}

int dcms_msg_unsubscribe(dcms_topic_id topic, dcms_msg_callback cb, void *user) {
    if (!g_ctx.inited)
        return dcms::kDcmsErrNotInit;
    if (!cb)
        return dcms::kDcmsErrInvalidArg;

    g_ctx.recv_thread->unsubscribe(topic, reinterpret_cast<std::uintptr_t>(cb), user);
    return dcms::kDcmsOk;
}

} // extern "C"
