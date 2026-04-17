// =============================================================================
// shm_defs.h — DCMS 共享内存层公共定义
//
// 本文件提供跨层共用的工具组件：
//   - SpinLock：基于 atomic<uint32_t> 的自旋锁，可放入 SHM 跨进程使用
//   - ShmBufHeader：可选的 per-allocation 控制头（引用计数、大小记录等）
//
// 注意：PAGE 级常量（BUDDY_PAGE_SHIFT / BUDDY_PAGE_SIZE 等）和
//       分配器结构体（bpage / sobj_ref 等）定义在：
//         buddy_allocator.h
//         slab_allocator.h
// =============================================================================
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace dcms {

// -----------------------------------------------------------------------------
// ShmBufHeader（可选）
//
// 若上层需要在每次 alloc 返回的指针前面隐藏控制信息，可使用此头。
// 布局（低地址 → 高地址）：
//   [ShmBufHeader 16B][用户数据]
//                      ↑ alloc() 返回此处
// -----------------------------------------------------------------------------
struct alignas(16) ShmBufHeader {
    std::atomic<std::int32_t> refcount;   ///< 引用计数，归零时触发回收
    std::uint32_t             buf_size;   ///< 整块（含本 header）的字节数
    std::uint8_t              is_slab;    ///< 1=Slab 分配；0=Buddy 直接分配
    std::uint8_t              slab_class; ///< Slab cache 索引（is_slab=1 时有效）
    std::uint8_t              _pad[6];    ///< 补齐至 16 字节

    ShmBufHeader(const ShmBufHeader &) = delete;
    ShmBufHeader &operator=(const ShmBufHeader &) = delete;
};
static_assert(sizeof(ShmBufHeader) == 16u, "ShmBufHeader 必须恰好 16 字节");

// -----------------------------------------------------------------------------
// SpinLock
//
// 基于 atomic<uint32_t> 的自旋锁，零初始化（SHM mmap 后内存为 0）即"未加锁"。
// 可以嵌入共享内存区域实现跨进程互斥。
// -----------------------------------------------------------------------------
struct SpinLock {
    std::atomic<std::uint32_t> state{0u}; ///< 0=未加锁，1=已加锁

    void lock() noexcept {
        std::uint32_t expected = 0u;
        while (!state.compare_exchange_weak(expected, 1u,
                   std::memory_order_acquire,
                   std::memory_order_relaxed)) {
            expected = 0u;
        }
    }

    void unlock() noexcept {
        state.store(0u, std::memory_order_release);
    }

    bool try_lock() noexcept {
        std::uint32_t expected = 0u;
        return state.compare_exchange_strong(expected, 1u,
                   std::memory_order_acquire,
                   std::memory_order_relaxed);
    }
};

/// RAII 自旋锁守卫
struct SpinGuard {
    explicit SpinGuard(SpinLock &lk) : lk_(lk) { lk_.lock(); }
    ~SpinGuard() { lk_.unlock(); }
    SpinLock &lk_;
};

} // namespace dcms
