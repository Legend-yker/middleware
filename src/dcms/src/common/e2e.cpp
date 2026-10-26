#include "common/e2e.h"

#include <cstring>

namespace dcms {

// e2e_crc 的计算范围：MsgMeta 前 40 字节（原始字段，不含 e2e_counter/e2e_crc）
// + payload 全部字节。这样任意字段被篡改都会导致 CRC 不匹配。
static constexpr std::size_t kMetaCrcBytes = 40u; // sizeof 原始 MsgMeta

void e2e_protect(MsgMeta       &meta,
                 const void    *payload,
                 std::uint32_t  payload_len,
                 std::uint32_t &counter_state) {
    meta.e2e_counter   = ++counter_state;
    meta.payload_len   = payload_len;
    // payload_crc32 不再单独计算：e2e_crc 的覆盖范围已包含整个 payload，
    // 重复算一次等于让每帧多扫一遍数据（发布侧原先要扫 2 遍），
    // 对点云这类大块数据这笔开销非常可观。字段保留供调试/兼容。
    meta.payload_crc32 = 0;
    // e2e_crc：覆盖 meta 前 40 字节 + payload
    std::uint32_t h = crc32(&meta, kMetaCrcBytes);
    meta.e2e_crc = crc32Update(h, payload, payload_len);
}

E2EResult e2e_check(const MsgMeta &meta,
                    const void    *payload,
                    std::uint32_t  payload_len,
                    E2EState      &state) {
    // Step1：验证 CRC
    std::uint32_t h        = crc32(&meta, kMetaCrcBytes);
    std::uint32_t expected = crc32Update(h, payload, payload_len);
    if (expected != meta.e2e_crc)
        return E2EResult::kCrcError;

    // Step2：验证 counter 连续性
    if (!state.initialized) {
        state.last_counter = meta.e2e_counter;
        state.initialized  = true;
        return E2EResult::kFirstMessage;
    }

    if (meta.e2e_counter != state.last_counter + 1u) {
        // 更新 last_counter 以继续追踪后续消息（不卡死在错误状态）
        state.last_counter = meta.e2e_counter;
        return E2EResult::kCounterError;
    }

    state.last_counter = meta.e2e_counter;
    return E2EResult::kOk;
}

const char *e2e_result_str(E2EResult r) {
    switch (r) {
    case E2EResult::kOk:           return "e2e_ok";
    case E2EResult::kCrcError:     return "e2e_crc_error";
    case E2EResult::kCounterError: return "e2e_counter_error";
    case E2EResult::kFirstMessage: return "e2e_first_message";
    default:                       return "e2e_unknown";
    }
}

} // namespace dcms
