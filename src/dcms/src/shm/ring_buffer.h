#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace dcms {

// Seqlock 协议说明（理解 write/read 的前提）：
//   seq 初始值 = 0（从未写入）
//   Publisher 写入前：seq +1 → 变为奇数，表示"写入中"
//   Publisher 写完后：seq +1 → 变为偶数（≥2），表示"可读"
//   Subscriber 读取：若 seq 为奇数则重试；读前读后 seq 相同才算成功
//   这样 Subscriber 无需持锁，只需两次原子读就能检测撕裂写（torn write）
struct alignas(8) RingSlotHeader {
    std::atomic<std::uint32_t> seq;          // seqlock 序号，偶数≥2=可读，奇数=写入中，0=从未写入
    std::uint32_t              payload_len;  // 本次实际写入的字节数（可小于 slot_size）
    std::uint64_t              timestamp_us; // 写入时的微秒时间戳
};
static_assert(sizeof(RingSlotHeader) == 16u, "RingSlotHeader must be 16 bytes");

// 环形缓冲区全局头，放在 shm 区域偏移 0 处，独占一个 cache line 避免伪共享
struct alignas(64) RingHeader {
    std::atomic<std::uint32_t> write_idx;   // 单调递增写游标，只有 Publisher 修改；对 ring_slots 取模得物理槽号
    std::uint32_t              ring_slots;  // 槽位总数，初始化后不变
    std::uint32_t              slot_size;   // 每槽 payload 容量（字节），初始化后不变
    std::uint8_t               _pad[52];    // 填满 64 字节，防止 Subscriber 读 write_idx 时与其他字段共享 cache line
};
static_assert(sizeof(RingHeader) == 64u, "RingHeader must be 64 bytes");

// 非持有型访问器：调用者负责 mmap，RingBuffer 只持有裸指针。
// 线程安全：write() 非线程安全（单写者），read() 与并发 write() 安全（seqlock 保证）。
// 满环策略：有损覆盖——写满后直接覆盖最旧槽，Subscriber 可能丢帧但不会阻塞 Publisher。
class RingBuffer {
public:
    RingBuffer() = default;
    RingBuffer(void *base, std::uint32_t ring_slots, std::uint32_t slot_size);

    // 将 payload 写入下一个槽（有损覆盖，不阻塞）。
    // 仅当 len > slot_size 时返回 false。
    bool write(const void *payload, std::uint32_t len);

    // 用 seqlock 读取 read_idx 对应槽的数据。
    // 返回 false：槽从未写入，或写入中（最多重试 8 次）。
    // out_seq 可供调用层检测自上次读取后槽是否被覆盖（seq 变化则数据已更新）。
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
