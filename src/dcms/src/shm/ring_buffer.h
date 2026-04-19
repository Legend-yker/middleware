#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace dcms {

// Slot header: seqlock protocol
//   even seq = slot being written (publisher holds it)
//   odd  seq = slot ready to read
struct alignas(8) RingSlotHeader {
    std::atomic<std::uint32_t> seq;
    std::uint32_t              payload_len;
    std::uint64_t              timestamp_us;
};
static_assert(sizeof(RingSlotHeader) == 16u, "RingSlotHeader must be 16 bytes");

// Ring header at offset 0 of the shm region (cache-line sized)
struct alignas(64) RingHeader {
    std::atomic<std::uint32_t> write_idx;   // monotonic; only Publisher writes
    std::uint32_t              ring_slots;  // immutable after init
    std::uint32_t              slot_size;   // payload capacity per slot, immutable
    std::uint8_t               _pad[52];
};
static_assert(sizeof(RingHeader) == 64u, "RingHeader must be 64 bytes");

// Non-owning accessor over an already-mmap'd region.
// Thread-safety: write() is NOT thread-safe (single-writer per topic).
//                read() is thread-safe with concurrent write().
class RingBuffer {
public:
    RingBuffer() = default;
    RingBuffer(void *base, std::uint32_t ring_slots, std::uint32_t slot_size);

    // Publisher: write payload into next slot (lossy ring, overwrites oldest)
    // Returns false only if len > slot_size_.
    bool write(const void *payload, std::uint32_t len);

    // Subscriber: read slot at read_idx using seqlock.
    // Returns false if slot is mid-write or no data at read_idx yet.
    bool read(std::uint32_t read_idx, void *out_buf, std::uint32_t buf_size,
              std::uint32_t &out_len, std::uint32_t &out_seq) const;

    RingHeader     *header()                  const;
    RingSlotHeader *slotHeader(std::uint32_t idx) const;
    void           *slotPayload(std::uint32_t idx) const;

    bool valid() const { return base_ != nullptr; }

private:
    std::size_t slotOffset(std::uint32_t idx) const;

    std::uint8_t  *base_{nullptr};
    std::uint32_t  ring_slots_{0};
    std::uint32_t  slot_size_{0};
};

// Helpers used by both Daemon and NodeHandle to agree on shm region size
inline std::uint64_t ringBufferSize(std::uint32_t ring_slots,
                                    std::uint32_t slot_size) {
    return static_cast<std::uint64_t>(sizeof(RingHeader)) +
           static_cast<std::uint64_t>(ring_slots) *
               (sizeof(RingSlotHeader) + slot_size);
}

inline std::uint64_t pageCeil(std::uint64_t n, std::uint64_t page = 4096u) {
    return (n + page - 1u) & ~(page - 1u);
}

} // namespace dcms
