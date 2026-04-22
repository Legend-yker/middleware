#pragma once

#include "dcms_export.h"

#include <cstddef>
#include <cstdint>

namespace dcms {

using TopicId = std::uint32_t; // topic 的唯一 ID，由 topic 名称哈希得到

// 消息类型：发布、RPC 请求、RPC 响应
enum class MsgType : std::uint8_t {
    kPublish = 0,
    kRequest = 1,
    kResponse = 2,
};

// 统一错误码，所有公共函数返回 int，0=成功，负数=失败
enum DcmsError : int {
    kDcmsOk = 0,
    kDcmsErrNotInit         = -1,  // 模块未初始化
    kDcmsErrBusy            = -2,  // 资源忙
    kDcmsErrShmFail         = -3,  // shm_open/mmap 失败
    kDcmsErrNotImpl         = -4,  // 功能未实现或协议错误
    kDcmsErrInvalidArg      = -5,  // 参数非法
    kDcmsErrCrcMismatch     = -6,  // CRC 校验失败
    kDcmsErrTimeout         = -7,  // 超时
    kDcmsErrNoSubscriber    = -8,  // 没有订阅者
    kDcmsErrDaemonNotFound  = -9,  // Daemon 未运行（connect 失败）
    kDcmsErrTopicExists     = -10, // topic 已有 Publisher，不可重复 advertise
};

DCMS_API const char *dcmsErrorToString(int err);

// 旧版消息头（保留兼容，当前 pub/sub 路径已改用 RingSlotHeader）
struct DcmsMsgHeader {
    std::uint32_t src_addr;
    std::uint32_t dst_addr;
    TopicId       topic_id;
    std::uint32_t seq;
    MsgType       msg_type;
    std::uint8_t  reserved[3];
    std::uint32_t crc32;
    std::uint64_t timestamp_transfer;
    std::uint64_t timestamp_system;
    std::uint64_t timestamp_publish;
    std::uint32_t data_offset; // payload 相对于本结构体起始的偏移
    std::uint32_t data_len;
};

static_assert(sizeof(DcmsMsgHeader) <= 128, "header too large");

// 通过 data_offset 直接取 payload 指针，避免调用方手算偏移
inline void *msgDataPtr(DcmsMsgHeader *hdr) {
    return reinterpret_cast<char *>(hdr) + hdr->data_offset;
}

inline const void *msgDataPtr(const DcmsMsgHeader *hdr) {
    return reinterpret_cast<const char *>(hdr) + hdr->data_offset;
}

} // namespace dcms
