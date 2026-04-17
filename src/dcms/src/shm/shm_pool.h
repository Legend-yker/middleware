#pragma once

#include "dcms_export.h"
#include "shm/buddy_allocator.h"
#include "shm/slab_allocator.h"

#include <cstddef>
#include <string>

// =============================================================================
// shm_pool.h — 共享内存池顶层封装
//
// 三个缓冲区（调用者提供 / ShmPool 内部管理）：
//   pool[]      — 被管理的共享内存（mmap 或 malloc 模拟），用户最终读写的地方
//   page_table  — pool 的管理元数据，每个 PAGE 一个 bpage，存在普通内存里
//   ref_array   — 对象级空闲链表节点，每个 PAGE 一个 sobj_ref，存在普通内存里
//
// page_table 和 ref_array 存在普通私有内存，不写共享内存，多进程安全。
// =============================================================================

namespace dcms {

class DCMS_API ShmPool {
public:
    static ShmPool &instance();

    ShmPool(const ShmPool &) = delete;
    ShmPool &operator=(const ShmPool &) = delete;

    /// 创建并初始化内存池（size 必须是 PAGE_SIZE 的整数倍）
    bool create(const std::string &name, std::size_t size);
    bool attach(const std::string &name);
    void destroy();

    /// 分配固定大小对象（obj_size 必须是 PAGE_SIZE 整数倍，且已在 create 时注册）
    /// 返回对象地址；未找到匹配 cache 时返回 nullptr
    void *alloc(std::size_t obj_size);

    /// 释放对象（只需指针，无需指定 cache，内部自动追溯）
    void free(void *ptr);

    /// 查询 pool 是否就绪
    bool ready() const { return region_ != nullptr; }

    std::size_t poolSize() const { return poolSize_; }

    /// 打印 buddy 和 slab 状态（调试用）
    void dump() const;

private:
    ShmPool();
    ~ShmPool();

    void *region_{nullptr};    ///< 被管理的内存区域（模拟 mmap 共享内存）
    std::size_t poolSize_{0};
    std::string shmName_;

    // buddy 层：page_table 是普通内存，与 region_ 分开存放
    struct buddy_zone zone_{};
    struct bpage     *page_table_{nullptr}; ///< 长度 = nr_pages

    // slab 层：ref_array 是进程私有内存，不写 region_
    struct slab_ctx   slab_ctx_{};
    struct sobj_ref  *ref_array_{nullptr};  ///< 长度 = nr_pages
};

} // namespace dcms
