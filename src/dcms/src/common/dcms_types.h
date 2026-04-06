#pragma once

#include "dcms_export.h"

#include <cstddef>
#include <cstdint>

namespace dcms {

using TopicId = std::uint32_t;

enum class MsgType : std::uint8_t {
    kPublish = 0,
    kRequest = 1,
    kResponse = 2,
};

enum DcmsError : int {
    kDcmsOk = 0,
    kDcmsErrNotInit = -1,
    kDcmsErrBusy = -2,
    kDcmsErrShmFail = -3,
    kDcmsErrNotImpl = -4,
    kDcmsErrInvalidArg = -5,
    kDcmsErrCrcMismatch = -6,
    kDcmsErrTimeout = -7,
    kDcmsErrNoSubscriber = -8,
};

DCMS_API const char *dcmsErrorToString(int err);

struct DcmsMsgHeader {
    std::uint32_t src_addr;
    std::uint32_t dst_addr;
    TopicId topic_id;
    std::uint32_t seq;
    MsgType msg_type;
    std::uint8_t reserved[3];
    std::uint32_t crc32;
    std::uint64_t timestamp_transfer;
    std::uint64_t timestamp_system;
    std::uint64_t timestamp_publish;
    std::uint32_t data_offset;
    std::uint32_t data_len;
};

static_assert(sizeof(DcmsMsgHeader) <= 128, "header too large");

inline void *msgDataPtr(DcmsMsgHeader *hdr) {
    return reinterpret_cast<char *>(hdr) + hdr->data_offset;
}

inline const void *msgDataPtr(const DcmsMsgHeader *hdr) {
    return reinterpret_cast<const char *>(hdr) + hdr->data_offset;
}

} // namespace dcms
