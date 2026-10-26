#include "shm/ring_buffer.h"
#include "common/dcms_msg.h"   // nowUs()
#include "shm/shm_pool.h"      // 槽位覆盖时回收旧消息

#include <cassert>
#include <cstring>

namespace dcms {

namespace {

// 槽格式为 [MsgMeta][ShmHandle] 时，槽位本身持有旧消息的一份引用（refcount 初始 1）。
// 覆盖槽位意味着旧消息不再被 ring 引用，这里归还这份引用；
// 仍有订阅者持有时 refcount 不会归零，等最后一个订阅者 recycle 后才真正释放。
// 订阅者只读 mmap，永远不会走到写路径，因此这里的回收只发生在发布侧。
inline ShmHandle takeOldHandle(void *slot_payload, std::uint32_t slot_size) {
    if (slot_size != kRingSlotPayloadSize) return ShmHandle::invalid();
    auto *start = static_cast<std::uint8_t *>(slot_payload);
    auto *h     = reinterpret_cast<ShmHandle *>(start + sizeof(MsgMeta));
    ShmHandle old = *h;
    if (old.valid()) *h = ShmHandle::invalid();  // 防止重复回收
    return old;
}

inline void recycleHandle(const ShmHandle &h) {
    if (h.valid()) ShmPool::instance().msg_recycle_handle(h);
}

} // namespace

RingBuffer::RingBuffer(void *base, std::uint32_t ring_slots,
                       std::uint32_t slot_size)
    : base_(static_cast<std::uint8_t *>(base)),
      ring_slots_(ring_slots),
      slot_size_(slot_size),
      retire_(ring_slots, ShmHandle::invalid()) {
    assert(base != nullptr);
    assert(ring_slots > 0);
    assert(slot_size > 0);
}

// 旧消息入队延迟回收：先把该位置上一圈写入的旧句柄真正归还，再放入本次的。
// 只有发布侧会走到这里（订阅侧只读 mmap）。
void RingBuffer::retireSlot(const ShmHandle &old) {
    const ShmHandle prev = retire_[retire_pos_];
    recycleHandle(prev);
    retire_[retire_pos_] = old;
    retire_pos_ = static_cast<std::uint32_t>((retire_pos_ + 1u) % retire_.size());
}

std::size_t RingBuffer::slotOffset(std::uint32_t idx) const {
    return sizeof(RingHeader) + static_cast<std::size_t>(idx) * ringSlotStride(slot_size_);
}

RingHeader *RingBuffer::header() const {
    return reinterpret_cast<RingHeader *>(base_);
}

RingSlotHeader *RingBuffer::slotHeader(std::uint32_t idx) const {
    return reinterpret_cast<RingSlotHeader *>(base_ + slotOffset(idx));
}

// 返回 slot 内 payload 起始地址（[MsgMeta 48B][ShmHandle 16B]）
void *RingBuffer::slotPayload(std::uint32_t idx) const {
    return base_ + slotOffset(idx) + sizeof(RingSlotHeader);
}

// ── 拷贝式写入（write / read）────────────────────────────────────────────────

bool RingBuffer::write(const void *payload, std::uint32_t len) {
    if (len > slot_size_) return false;

    RingHeader *hdr = header();
    const std::uint32_t widx =
        hdr->write_idx.load(std::memory_order_relaxed) % ring_slots_;

    RingSlotHeader *sh = slotHeader(widx);
    sh->seq.fetch_add(1u, std::memory_order_release);  // → 奇数

    const ShmHandle old = takeOldHandle(slotPayload(widx), slot_size_);
    std::memcpy(slotPayload(widx), payload, len);
    sh->payload_len  = len;
    sh->timestamp_us = nowUs();

    retireSlot(old);
    sh->seq.fetch_add(1u, std::memory_order_release);  // → 偶数
    hdr->write_idx.fetch_add(1u, std::memory_order_release);
    return true;
}

bool RingBuffer::read(std::uint32_t read_idx, void *out_buf,
                      std::uint32_t buf_size, std::uint32_t &out_len,
                      std::uint32_t &out_seq) const {
    const std::uint32_t sidx = read_idx % ring_slots_;
    RingSlotHeader *sh = slotHeader(sidx);

    constexpr int kMaxRetry = 8;
    for (int i = 0; i < kMaxRetry; ++i) {
        std::uint32_t seq1 = sh->seq.load(std::memory_order_acquire);
        if (seq1 == 0u) return false;       // 从未写入
        if (seq1 & 1u)  continue;           // 写入中

        const std::uint32_t copy_len =
            (sh->payload_len < buf_size) ? sh->payload_len : buf_size;
        std::memcpy(out_buf, slotPayload(sidx), copy_len);

        std::uint32_t seq2 = sh->seq.load(std::memory_order_acquire);
        if (seq1 == seq2) {
            out_len = sh->payload_len;
            out_seq = seq1;
            return true;
        }
    }
    return false;
}

// ── 零拷贝写入 ────────────────────────────────────────────────────────────────

RingBuffer::WriteHandle RingBuffer::beginWrite() {
    WriteHandle h{};
    if (!base_) return h;

    RingHeader *hdr = header();
    const std::uint32_t widx =
        hdr->write_idx.load(std::memory_order_relaxed) % ring_slots_;

    RingSlotHeader *sh = slotHeader(widx);
    sh->seq.fetch_add(1u, std::memory_order_release);  // → 奇数

    auto *slot_start = static_cast<std::uint8_t *>(slotPayload(widx));
    h.meta       = reinterpret_cast<MsgMeta *>(slot_start);
    h.shm_handle = reinterpret_cast<ShmHandle *>(slot_start + sizeof(MsgMeta));
    h.slot_hdr   = sh;
    h.old_handle = takeOldHandle(slot_start, slot_size_);
    h.valid      = true;
    return h;
}

void RingBuffer::endWrite(WriteHandle &handle, std::uint32_t data_len) {
    if (!handle.valid) return;
    handle.slot_hdr->payload_len  = sizeof(MsgMeta) + data_len;
    handle.slot_hdr->timestamp_us = nowUs();
    handle.slot_hdr->seq.fetch_add(1u, std::memory_order_release);  // → 偶数
    header()->write_idx.fetch_add(1u, std::memory_order_release);
    retireSlot(handle.old_handle);
    handle.valid = false;
}

void RingBuffer::cancelWrite(WriteHandle &handle) {
    if (!handle.valid) return;
    // 取消发布时槽内 handle 已失效，旧消息同样入队回收，避免泄漏
    retireSlot(handle.old_handle);
    handle.slot_hdr->payload_len = 0;
    handle.slot_hdr->seq.fetch_add(1u, std::memory_order_release);  // → 偶数，不推进 write_idx
    handle.valid = false;
}

// ── 零拷贝读取 ────────────────────────────────────────────────────────────────

void RingBuffer::releaseAll() {
    if (!base_ || slot_size_ != kRingSlotPayloadSize) return;

    for (std::uint32_t i = 0; i < ring_slots_; ++i) {
        for (int retry = 0; retry < 8; ++retry) {
            const std::uint32_t seq1 =
                slotHeader(i)->seq.load(std::memory_order_acquire);
            if (seq1 & 1u) continue;                    // 写入中，稍等
            auto *start = static_cast<std::uint8_t *>(slotPayload(i));
            auto *h     = reinterpret_cast<ShmHandle *>(start + sizeof(MsgMeta));
            const ShmHandle old = *h;
            if (old.valid()) { *h = ShmHandle::invalid(); recycleHandle(old); }
            break;
        }
    }
    for (auto &h : retire_) {
        recycleHandle(h);
        h = ShmHandle::invalid();
    }
}

RingBuffer::ReadView RingBuffer::readView(std::uint32_t read_idx) const {
    ReadView view{};
    const std::uint32_t sidx = read_idx % ring_slots_;
    RingSlotHeader *sh = slotHeader(sidx);

    constexpr int kMaxRetry = 8;
    for (int i = 0; i < kMaxRetry; ++i) {
        std::uint32_t seq = sh->seq.load(std::memory_order_acquire);
        if (seq == 0u) return view;   // 从未写入
        if (seq & 1u)  continue;      // 写入中

        auto *start   = static_cast<std::uint8_t *>(slotPayload(sidx));
        view.meta        = reinterpret_cast<const MsgMeta *>(start);
        view.shm_handle  = reinterpret_cast<const ShmHandle *>(start + sizeof(MsgMeta));
        view.seq_snapshot = seq;
        return view;
    }
    return view;
}

} // namespace dcms
