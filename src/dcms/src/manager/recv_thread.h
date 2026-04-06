#pragma once

#include "dcms_export.h"
#include "common/dcms_types.h"
#include "transport/common/ipc_backend.h"

#include <atomic>
#include <functional>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace dcms {

using MsgCallback = std::function<void(const DcmsMsgHeader *hdr, const void *data, std::size_t len)>;

class DCMS_API RecvThread {
public:
    explicit RecvThread(std::shared_ptr<IpcBackend> backend);
    ~RecvThread();

    RecvThread(const RecvThread &) = delete;
    RecvThread &operator=(const RecvThread &) = delete;

    void subscribe(TopicId topic, std::uintptr_t callback_key, void *user_key, MsgCallback cb);
    void unsubscribe(TopicId topic, std::uintptr_t callback_key, void *user_key);

    bool start();
    void stop();

private:
    struct SubscriptionEntry {
        std::uintptr_t callback_key;
        void *user_key;
        MsgCallback cb;
    };

    void loop();
    void dispatch(const DcmsMsgHeader *hdr);

    std::shared_ptr<IpcBackend> backend_;
    std::atomic<bool> running_{false};
    std::thread worker_;

    std::mutex sub_mutex_;
    std::unordered_map<TopicId, std::vector<SubscriptionEntry>> subs_;
};

} // namespace dcms
