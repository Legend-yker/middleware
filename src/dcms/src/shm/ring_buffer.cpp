#include "shm/ring_buffer.h"
#include "common/dcms_msg.h"   // nowUs()

#include <algorithm>
#include <cassert>
#include <cstring>

namespace dcms {

RingBuffer::RingBuffer(void *base, std::uint32_t ring_slots,
                       std::uint32_t slot_size)
    : base_(static_cast<std::uint8_t *>(base)),
      ring_slots_(ring_slots),
      slot_size_(slot_size) {
    assert(base != nullptr);
    assert(ring_slots > 0);
    assert(slot_size > 0);
}

std::size_t RingBuffer::slotOffset(std::uint32_t idx) const {
    return sizeof(RingHeader) +
           static_cast<std::size_t>(idx) * (sizeof(RingSlotHeader) + slot_size_);
}

RingHeader *RingBuffer::header() const {
    return reinterpret_cast<RingHeader *>(base_);
}

RingSlotHeader *RingBuffer::slotHeader(std::uint32_t idx) const {
    return reinterpret_cast<RingSlotHeader *>(base_ + slotOffset(idx));
}

void *RingBuffer::slotPayload(std::uint32_t idx) const {
    return base_ + slotOffset(idx) + sizeof(RingSlotHeader);
}

bool RingBuffer::write(const void *payload, std::uint32_t len) {
    if (len > slot_size_)
        return false;

    RingHeader *hdr = header();
    // write_idx 只有本线程修改，relaxed 读即可（不需要同步其他线程的写）
    const std::uint32_t widx =
        hdr->write_idx.load(std::memory_order_relaxed) % ring_slots_;

    RingSlotHeader *sh = slotHeader(widx);

    // Step1：seq +1 → 奇数，通知 Subscriber "此槽写入中，请勿读取"
    // release 保证此 store 对所有后续 acquire 可见
    sh->seq.fetch_add(1u, std::memory_order_release);

    std::memcpy(slotPayload(widx), payload, len);
    sh->payload_len  = len;
    sh->timestamp_us = nowUs();

    // Step2：seq +1 → 偶数，通知 Subscriber "数据就绪，可以读取"
    sh->seq.fetch_add(1u, std::memory_order_release);

    // Step3：推进全局写游标；release 保证 Subscriber acquire 此值后能看到 slot 内的完整写入
    hdr->write_idx.fetch_add(1u, std::memory_order_release);
    return true;
}

bool RingBuffer::read(std::uint32_t read_idx, void *out_buf,
                      std::uint32_t buf_size, std::uint32_t &out_len,
                      std::uint32_t &out_seq) const {
    const std::uint32_t sidx = read_idx % ring_slots_;
    RingSlotHeader *sh = slotHeader(sidx);

    // seqlock 读取协议：
    //   seq == 0        → 此槽从未被写入，直接放弃
    //   seq 为奇数       → Publisher 正在写入，等待重试
    //   seq 为偶数(≥2)   → 数据就绪，可以 memcpy
    //   memcpy 前后 seq 不一致 → 发生撕裂读（Publisher 在读取中间又写了一次），重试
    constexpr int kMaxRetry = 8;
    for (int i = 0; i < kMaxRetry; ++i) {
        std::uint32_t seq1 = sh->seq.load(std::memory_order_acquire); // acquire：确保后续读 payload 不被重排到此之前
        if (seq1 == 0u)
            return false;           // 槽从未写入
        if ((seq1 & 1u) != 0u)
            continue;               // 写入中，重试

        const std::uint32_t copy_len =
            std::min(sh->payload_len, buf_size);
        std::memcpy(out_buf, slotPayload(sidx), copy_len);

        std::uint32_t seq2 = sh->seq.load(std::memory_order_acquire); // 再次检查：读期间 Publisher 有没有又写了一次
        if (seq1 == seq2) {
            // 两次 seq 相同 → memcpy 期间没有并发写，数据完整
            out_len = sh->payload_len;
            out_seq = seq1;
            return true;
        }
        // seq 变了 → 撕裂读，重试
    }
    return false;
}

} // namespace dcms
