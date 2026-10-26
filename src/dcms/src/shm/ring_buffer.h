#pragma once

#include "common/dcms_msg.h"
#include "shm/shm_defs.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace dcms {

// Seqlock 协议（Publisher 写入 / Subscriber 读取的同步核心）：
//   seq 初始值 = 0（从未写入）
//   写入前：seq +1 → 奇数（"写入中"）
//   写完后：seq +1 → 偶数（≥2，"可读"）
//   Subscriber 读取：seq 为奇数则重试；前后 seq 不一致则重试（防撕裂读）
struct alignas(8) RingSlotHeader {
    std::atomic<std::uint32_t> seq;          // seqlock 序号
    std::uint32_t              payload_len;  // 本次实际写入的字节数
    std::uint64_t              timestamp_us; // 写入时的微秒时间戳
};
static_assert(sizeof(RingSlotHeader) == 16u, "RingSlotHeader must be 16 bytes");

// 环形缓冲区全局头，独占一个 cache line 避免伪共享
struct alignas(64) RingHeader {
    std::atomic<std::uint32_t> write_idx;   // 单调递增写游标（只有 Publisher 修改）
    std::uint32_t              ring_slots;  // 槽位总数，初始化后不变
    std::uint32_t              slot_size;   // 每槽 payload 容量（字节），初始化后不变
    std::uint8_t               _pad[52];
};
static_assert(sizeof(RingHeader) == 64u, "RingHeader must be 64 bytes");

// =============================================================================
// 新版槽格式（对齐公司生产版零拷贝架构）：
//
//   [RingSlotHeader 16B][MsgMeta 48B][ShmHandle 16B]
//        ↑ seqlock         ↑ 元数据     ↑ 指向 ShmPool 中实际用户数据
//
// slot_size = sizeof(MsgMeta) + sizeof(ShmHandle) = 64（对所有 topic 固定）
// 实际用户数据（T）存储在 ShmPool 中，通过 ShmHandle.offset 访问，零拷贝。
//
// 与旧版的差异：旧版 slot_size = sizeof(MsgMeta) + sizeof(T)，用户数据直接存在槽内。
// =============================================================================

/// 所有 topic 共用的固定 ring slot payload 大小
static constexpr std::uint32_t kRingSlotPayloadSize =
    static_cast<std::uint32_t>(sizeof(MsgMeta) + sizeof(ShmHandle));

// 非持有型访问器：调用者负责 mmap，RingBuffer 只持有裸指针。
class RingBuffer {
public:
    RingBuffer() = default;
    RingBuffer(void *base, std::uint32_t ring_slots, std::uint32_t slot_size);

    // 将 payload 写入下一个槽（有损覆盖，不阻塞）。len > slot_size 时返回 false。
    bool write(const void *payload, std::uint32_t len);

    // 用 seqlock 读取 read_idx 对应槽的数据（memcpy 到 out_buf）。
    bool read(std::uint32_t read_idx, void *out_buf, std::uint32_t buf_size,
              std::uint32_t &out_len, std::uint32_t &out_seq) const;

    RingHeader     *header()                  const;
    RingSlotHeader *slotHeader(std::uint32_t idx) const;
    void           *slotPayload(std::uint32_t idx) const;

    bool          valid()    const { return base_ != nullptr; }
    std::uint32_t slots()    const { return ring_slots_; }
    std::uint32_t slotSize() const { return slot_size_; }

    // ── 零拷贝写入 API ─────────────────────────────────────────────────────────
    //
    // 使用方式（Publisher 侧）：
    //   auto wh = ring_.beginWrite();           // 1. 抢占槽（seq→奇数）
    //   wh.meta->seq = ...;                     // 2. 填写 MsgMeta（在 ring slot 里）
    //   *wh.shm_handle = pool.alloc_handle(N);  // 3. 将 ShmHandle 写入 ring slot
    //   ring_.endWrite(wh, sizeof(ShmHandle));  // 4. 提交（seq→偶数，推进 write_idx）
    //
    // 用户数据直接写入 ShmPool（pool.handle_payload(handle) 返回的地址），不经过 ring。
    struct WriteHandle {
        MsgMeta        *meta{nullptr};       ///< 指向 ring slot 内 MsgMeta 区域
        ShmHandle      *shm_handle{nullptr}; ///< 指向 ring slot 内 ShmHandle 区域
        RingSlotHeader *slot_hdr{nullptr};
        ShmHandle       old_handle{ShmHandle::invalid()}; ///< 覆盖前槽内旧消息（endWrite 时回收）
        bool            valid{false};
    };

    WriteHandle beginWrite();
    void endWrite(WriteHandle &handle, std::uint32_t data_len);
    void cancelWrite(WriteHandle &handle);

    // ── 零拷贝读取 API ─────────────────────────────────────────────────────────
    //
    // 使用方式（Subscriber 侧）：
    //   auto view = ring_.readView(read_idx_);     // 1. 取只读视图（不 memcpy）
    //   if (!view.valid()) break;
    //   ShmHandle h = *view.shm_handle;            // 2. 取 ShmHandle
    //   const T* payload = (T*)pool.handle_payload(h); // 3. 直接访问 ShmPool 数据
    //   if (view.stale(...)) log("overwritten");   // 4. 检测是否被覆写
    //
    // 注意：readView 取得的是 ring slot 的只读指针，回调结束后 slot 随时可被覆写。
    struct ReadView {
        const MsgMeta  *meta{nullptr};       ///< ring slot 内 MsgMeta（只读）
        const ShmHandle *shm_handle{nullptr};///< ring slot 内 ShmHandle（只读）
        std::uint32_t   seq_snapshot{0};

        bool valid() const { return meta != nullptr; }

        bool stale(const RingSlotHeader *sh) const {
            return sh->seq.load(std::memory_order_acquire) != seq_snapshot;
        }
    };

    // ── 销毁时释放本 ring 持有的全部消息引用 ────────────────────────────────
    // Publisher 析构时调用：槽里的残留帧和 retire 队列都不再有人消费，
    // 必须显式归还，否则每个被销毁的 topic 都会永久漏掉约 2×ring_slots 份数据。
    void releaseAll();

    ReadView readView(std::uint32_t read_idx) const;

    // 移动语义（RingBuffer 本身只是一个非拥有访问器）
    RingBuffer(RingBuffer &&o) noexcept = default;
    RingBuffer &operator=(RingBuffer &&o) noexcept = default;

private:
    std::size_t slotOffset(std::uint32_t idx) const;

    // 延迟回收：被覆盖的消息先进入 retire 环形队列，绕完一圈（ring_slots_ 次
    // 写入）后才真正归还 refcount。这样：
    //   - 内存占用被限制在约 2 圈以内（有泄漏修复，又不会无限增长）
    //   - 落后不超过一圈的慢订阅者仍能安全 acquire
    //   - 落后超过一圈的订阅者本来就在读被覆盖的旧数据（有损语义）
    void retireSlot(const ShmHandle &old);

    std::uint8_t  *base_{nullptr};
    std::uint32_t  ring_slots_{0};
    std::uint32_t  slot_size_{0};

    std::vector<ShmHandle> retire_;      ///< 待回收队列，长度 = ring_slots_
    std::uint32_t          retire_pos_{0};
};

// ── Size helpers ────────────────────────────────────────────────────────────

// 槽步长向上对齐到 64B（一条 cache line）。
//
// 不对齐时（16B 头 + 64B payload = 80B），相邻槽会跨 cache line，且“我的写游标”
// 会和“邻居槽的数据”落在同一条 line 上：发布者与订阅者、不同槽之间会互相
// 制造伪共享，多核上表现为延迟抖动。对齐后每个槽独占整数条 line。
inline constexpr std::size_t ringSlotStride(std::uint32_t slot_size) {
    const std::size_t raw = sizeof(RingSlotHeader) + slot_size;
    return (raw + 63u) & ~static_cast<std::size_t>(63u);
}

inline std::uint64_t ringBufferSize(std::uint32_t ring_slots,
                                    std::uint32_t slot_size) {
    return static_cast<std::uint64_t>(sizeof(RingHeader)) +
           static_cast<std::uint64_t>(ring_slots) * ringSlotStride(slot_size);
}

inline std::uint64_t pageCeil(std::uint64_t n, std::uint64_t page = 4096u) {
    return (n + page - 1u) & ~(page - 1u);
}

} // namespace dcms
