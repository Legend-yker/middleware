#include "api/publisher.h"
#include "api/dcms_api.h"
#include "common/crc32.h"

#include <cstring>
#include <functional>
#include <vector>

namespace dcms {

Publisher::Publisher(TopicId topic) : topic_(topic) {}

Publisher::Publisher(const std::string &topic_name)
    : topic_(static_cast<TopicId>(std::hash<std::string>{}(topic_name))) {}

int Publisher::publish(const MsgMeta &meta, const void *payload,
                       std::uint32_t payload_len) {
    // ---- C++ layer: CRC integrity check ----
    if (payload_len > 0 && meta.payload_crc32 != 0) {
        std::uint32_t actual = crc32(payload, payload_len);
        if (actual != meta.payload_crc32)
            return kDcmsErrCrcMismatch;
    }

    // ---- Pack [MsgMeta][payload] into contiguous buffer for C API ----
    const std::uint32_t total =
        static_cast<std::uint32_t>(sizeof(MsgMeta)) + payload_len;

    constexpr std::uint32_t kStackThreshold = 4096;
    if (total <= kStackThreshold) {
        std::uint8_t buf[kStackThreshold];
        std::memcpy(buf, &meta, sizeof(MsgMeta));
        if (payload_len > 0)
            std::memcpy(buf + sizeof(MsgMeta), payload, payload_len);
        return dcms_msg_publish(topic_, buf, total);
    }

    std::vector<std::uint8_t> buf(total);
    std::memcpy(buf.data(), &meta, sizeof(MsgMeta));
    if (payload_len > 0)
        std::memcpy(buf.data() + sizeof(MsgMeta), payload, payload_len);
    return dcms_msg_publish(topic_, buf.data(), total);
}

} // namespace dcms
