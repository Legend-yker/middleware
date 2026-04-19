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
    // Only the publisher reads/modifies write_idx, so relaxed load is fine.
    const std::uint32_t widx =
        hdr->write_idx.load(std::memory_order_relaxed) % ring_slots_;

    RingSlotHeader *sh = slotHeader(widx);

    // Seqlock: mark slot as "being written" (seq → even)
    sh->seq.fetch_add(1u, std::memory_order_release);

    std::memcpy(slotPayload(widx), payload, len);
    sh->payload_len  = len;
    sh->timestamp_us = nowUs();

    // Seqlock: mark slot as "ready" (seq → odd)
    sh->seq.fetch_add(1u, std::memory_order_release);

    // Advance global write cursor (release so subscribers see the slot write)
    hdr->write_idx.fetch_add(1u, std::memory_order_release);
    return true;
}

bool RingBuffer::read(std::uint32_t read_idx, void *out_buf,
                      std::uint32_t buf_size, std::uint32_t &out_len,
                      std::uint32_t &out_seq) const {
    const std::uint32_t sidx = read_idx % ring_slots_;
    RingSlotHeader *sh = slotHeader(sidx);

    // Seqlock convention used by write():
    //   seq=0          → slot never written (initial state)
    //   seq odd (1,3…) → write in progress
    //   seq even ≥ 2   → data ready
    constexpr int kMaxRetry = 8;
    for (int i = 0; i < kMaxRetry; ++i) {
        std::uint32_t seq1 = sh->seq.load(std::memory_order_acquire);
        if (seq1 == 0u)
            return false;           // never written
        if ((seq1 & 1u) != 0u)
            continue;               // write in progress, retry

        const std::uint32_t copy_len =
            std::min(sh->payload_len, buf_size);
        std::memcpy(out_buf, slotPayload(sidx), copy_len);

        std::uint32_t seq2 = sh->seq.load(std::memory_order_acquire);
        if (seq1 == seq2) {
            out_len = sh->payload_len;
            out_seq = seq1;
            return true;
        }
        // torn read: retry
    }
    return false;
}

} // namespace dcms
