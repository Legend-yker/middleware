#pragma once

#include "dcms_export.h"
#include "common/dcms_msg.h"
#include "common/dcms_types.h"

#include <cstdint>
#include <string>

namespace dcms {

class DCMS_API Publisher {
public:
    explicit Publisher(TopicId topic);
    explicit Publisher(const std::string &topic_name);

    int publish(const MsgMeta &meta, const void *payload, std::uint32_t payload_len);

    template <typename T>
    int publish(const Msg<T> &msg) {
        return publish(msg.meta, &msg.data, static_cast<std::uint32_t>(sizeof(T)));
    }

    TopicId topic() const { return topic_; }

private:
    TopicId topic_;
};

} // namespace dcms
