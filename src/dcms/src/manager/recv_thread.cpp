#include "manager/recv_thread.h"
#include "common/crc32.h"
#include "shm/shm_pool.h"

#include <algorithm>
#include <cstring>

namespace dcms {

RecvThread::RecvThread(std::shared_ptr<IpcBackend> backend)
    : backend_(std::move(backend)) {}

RecvThread::~RecvThread() { stop(); }

void RecvThread::subscribe(TopicId topic, std::uintptr_t callback_key,
                           void *user_key, MsgCallback cb) {
    std::lock_guard<std::mutex> lk(sub_mutex_);
    subs_[topic].push_back(SubscriptionEntry{
        callback_key,
        user_key,
        std::move(cb),
    });
}

void RecvThread::unsubscribe(TopicId topic, std::uintptr_t callback_key, void *user_key) {
    std::lock_guard<std::mutex> lk(sub_mutex_);
    auto it = subs_.find(topic);
    if (it == subs_.end())
        return;

    auto &entries = it->second;
    entries.erase(
        std::remove_if(entries.begin(), entries.end(),
                       [&](const SubscriptionEntry &entry) {
                           return entry.callback_key == callback_key &&
                                  entry.user_key == user_key;
                       }),
        entries.end());

    if (entries.empty())
        subs_.erase(it);
}

bool RecvThread::start() {
    if (running_.exchange(true))
        return false;
    worker_ = std::thread([this] { loop(); });
    return true;
}

void RecvThread::stop() {
    running_ = false;
    if (worker_.joinable())
        worker_.join();
}

void RecvThread::loop() {
    while (running_.load(std::memory_order_relaxed)) {
        void *ptr = nullptr;
        std::size_t len = 0;
        if (!backend_->recvNotify(ptr, len, 50))
            continue;
        if (!ptr || len < sizeof(DcmsMsgHeader))
            continue;

        auto *hdr = static_cast<DcmsMsgHeader *>(ptr);

        std::uint32_t expected = hdr->crc32;
        hdr->crc32 = 0;
        std::uint32_t actual = crc32(hdr, sizeof(DcmsMsgHeader) + hdr->data_len);
        hdr->crc32 = expected;
        if (actual != expected)
            continue;

        dispatch(hdr);

        ShmPool::instance().free(ptr);
    }
}

void RecvThread::dispatch(const DcmsMsgHeader *hdr) {
    std::lock_guard<std::mutex> lk(sub_mutex_);
    auto it = subs_.find(hdr->topic_id);
    if (it == subs_.end())
        return;
    const void *data = msgDataPtr(hdr);
    for (auto &entry : it->second)
        entry.cb(hdr, data, hdr->data_len);
}

} // namespace dcms
