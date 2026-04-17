// =============================================================================
// shm_pool.cpp — 共享内存池顶层封装实现
//
// 当前用 malloc 模拟 mmap 共享内存区域，后续可替换为 shm_open + mmap。
//
// 初始化顺序：
//   1. 分配 region（模拟 mmap pool）
//   2. 分配 page_table（普通内存，长度 = nr_pages 个 bpage）
//   3. 分配 ref_array（普通内存，长度 = nr_pages 个 sobj_ref）
//   4. buddy_zone_init → 自举建立 buddy 空闲树
//   5. slab_ctx_init   → 绑定 buddy zone 和 ref_array
//   6. 注册若干默认 slab cache（PAGE_SIZE 的整数倍大小）
// =============================================================================

#include "shm/shm_pool.h"

#include <cstdlib>
#include <cstdio>
#include <cstring>

namespace dcms {

ShmPool::ShmPool() = default;

ShmPool::~ShmPool() { destroy(); }

ShmPool &ShmPool::instance() {
    static ShmPool inst;
    return inst;
}

bool ShmPool::create(const std::string &name, std::size_t size) {
    if (region_) return false; // 已经初始化

    // size 必须是 PAGE_SIZE 整数倍，否则 buddy_zone_init 会失败
    if (size == 0 || (size & (BUDDY_PAGE_SIZE - 1u)) != 0) return false;

    // ------------------------------------------------------------------
    // 步骤1：分配模拟共享内存区域（pool）
    // 后续替换为 shm_open + mmap；aligned_alloc 保证页对齐
    // ------------------------------------------------------------------
    void *mem = std::aligned_alloc(BUDDY_PAGE_SIZE, size);
    if (!mem) return false;
    std::memset(mem, 0, size);

    uint32_t nr_pages = (uint32_t)(size >> BUDDY_PAGE_SHIFT);

    // ------------------------------------------------------------------
    // 步骤2：分配 page_table（普通内存，不在共享内存里）
    // page_table[pfn] 是 pool 中第 pfn 个 4KB PAGE 的管理元数据
    // ------------------------------------------------------------------
    auto *ptable = static_cast<struct bpage *>(
        std::malloc(sizeof(struct bpage) * nr_pages));
    if (!ptable) {
        std::free(mem);
        return false;
    }
    std::memset(ptable, 0, sizeof(struct bpage) * nr_pages);

    // ------------------------------------------------------------------
    // 步骤3：分配 ref_array（进程私有普通内存，绝不写入共享内存）
    // ref_array[pfn] 是 pfn 对应 PAGE 上对象的空闲链表节点
    // 这是与标准 Linux slab 最大的差异：空闲链表完全在私有内存，
    // 多进程 mmap 同一块 pool 时各自维护自己的 ref_array，互不干扰。
    // ------------------------------------------------------------------
    auto *rarray = static_cast<struct sobj_ref *>(
        std::malloc(sizeof(struct sobj_ref) * nr_pages));
    if (!rarray) {
        std::free(ptable);
        std::free(mem);
        return false;
    }

    // ------------------------------------------------------------------
    // 步骤4：初始化 buddy zone（自举建立空闲树）
    // ------------------------------------------------------------------
    if (!buddy_zone_init(&zone_,
                         static_cast<uint8_t *>(mem),
                         size,
                         ptable)) {
        std::free(rarray);
        std::free(ptable);
        std::free(mem);
        return false;
    }

    // ------------------------------------------------------------------
    // 步骤5：初始化 slab ctx
    // ------------------------------------------------------------------
    if (!slab_ctx_init(&slab_ctx_, &zone_, rarray, nr_pages)) {
        std::free(rarray);
        std::free(ptable);
        std::free(mem);
        return false;
    }

    // ------------------------------------------------------------------
    // 步骤6：注册默认 slab cache
    // 对象大小必须是 PAGE_SIZE 整数倍，page_order 满足 objs/slab >= 4
    //   4KB 对象：page_order=2 → slab=16KB，4 个对象/slab
    //   8KB 对象：page_order=3 → slab=32KB，4 个对象/slab
    //  16KB 对象：page_order=4 → slab=64KB，4 个对象/slab
    // ------------------------------------------------------------------
    slab_cache_create(&slab_ctx_, "obj_4k",  4096,  2);
    slab_cache_create(&slab_ctx_, "obj_8k",  8192,  3);
    slab_cache_create(&slab_ctx_, "obj_16k", 16384, 4);

    region_     = mem;
    poolSize_   = size;
    shmName_    = name;
    page_table_ = ptable;
    ref_array_  = rarray;

    return true;
}

bool ShmPool::attach(const std::string &name) {
    // stub：同进程内 create 即 attach
    (void)name;
    return region_ != nullptr;
}

void ShmPool::destroy() {
    std::free(ref_array_);
    ref_array_ = nullptr;
    std::free(page_table_);
    page_table_ = nullptr;
    std::free(region_);
    region_   = nullptr;
    poolSize_ = 0;
    std::memset(&zone_,     0, sizeof(zone_));
    std::memset(&slab_ctx_, 0, sizeof(slab_ctx_));
}

void *ShmPool::alloc(std::size_t obj_size) {
    if (!region_) return nullptr;

    // 在已注册的 slab cache 中找匹配 obj_size 的缓存
    for (int32_t i = 0; i < slab_ctx_.ncaches; i++) {
        struct slab_cache *c = &slab_ctx_.caches[i];
        if (c->obj_size == (int32_t)obj_size)
            return slab_alloc(&slab_ctx_, c);
    }

    // 没有匹配 cache：回落到 buddy 直接分配（按页对齐向上取整）
    // 计算需要几阶：找最小 order 使 2^order * PAGE_SIZE >= obj_size
    if (obj_size == 0) return nullptr;
    uint32_t order = 0;
    size_t   block = BUDDY_PAGE_SIZE;
    while (block < obj_size && order < BUDDY_MAX_ORDER - 1u) {
        order++;
        block <<= 1u;
    }
    struct bpage *page = buddy_alloc(&zone_, order);
    if (!page) return nullptr;
    return bpage_to_addr(&zone_, page);
}

void ShmPool::free(void *ptr) {
    if (!region_ || !ptr) return;

    // 判断 ptr 是否在某个 slab page 内（PG_SLAB 标志）
    struct bpage *page = addr_to_bpage(&zone_, static_cast<uint8_t *>(ptr));
    if (page->flags & PG_SLAB) {
        // slab_free 自动追溯 cache，不需要调用者指定
        slab_free(&slab_ctx_, ptr);
    } else {
        // buddy 直接分配的块，直接归还
        buddy_free(&zone_, page);
    }
}

void ShmPool::dump() const {
    buddy_dump(&zone_);
    slab_dump(&slab_ctx_);
}

} // namespace dcms
