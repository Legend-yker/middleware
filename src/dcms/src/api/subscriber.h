#pragma once

#include "api/dcms_api.h"
#include "common/dcms_msg.h"
#include "common/dcms_types.h"

#include <cstring>
#include <functional>
#include <string>

namespace dcms {

template <typename T>
class Subscriber {
public:
    using Callback = std::function<void(const MsgMeta &meta, const T &data)>;

    Subscriber(TopicId topic, Callback cb) : topic_(topic), cb_(std::move(cb)) {
        dcms_msg_subscribe(topic_, &Subscriber::cCallback, this);
    }

    ~Subscriber() {
        dcms_msg_unsubscribe(topic_, &Subscriber::cCallback, this);
    }

    Subscriber(const std::string &topic_name, Callback cb)
        : Subscriber(static_cast<TopicId>(std::hash<std::string>{}(topic_name)),
                     std::move(cb)) {}

    Subscriber(const Subscriber &) = delete;
    Subscriber &operator=(const Subscriber &) = delete;

    TopicId topic() const { return topic_; }

private:
    static void cCallback(dcms_topic_id /*topic*/, const void *data,
                          std::uint32_t len, void *user) {
        auto *self = static_cast<Subscriber *>(user);
        if (!self->cb_)
            return;

        const std::uint32_t required = sizeof(MsgMeta) + sizeof(T);
        if (len >= required) {
            const auto *meta = static_cast<const MsgMeta *>(data);
            const void *payload =
                static_cast<const char *>(data) + sizeof(MsgMeta);
            T msg;
            std::memcpy(&msg, payload, sizeof(T));
            self->cb_(*meta, msg);
        }
    }

    TopicId topic_;
    Callback cb_;
};

} // namespace dcms
