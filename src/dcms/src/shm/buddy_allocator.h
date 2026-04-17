#pragma once

#include <cstddef>
#include <cstdint>

// =============================================================================
// buddy_allocator.h — Buddy 内存分配器
//
// 整体架构（三层，本文件实现最底层）：
//   用户代码  →  Slab 层  →  Buddy 层  →  pool[]
//
// Buddy 按 2 的幂次管理一片连续的字节数组（pool），负责大块内存的分配与合并。
// 设计参考 Linux 内核 mm/ 子系统，针对共享内存做了简化。
// =============================================================================

namespace dcms {

// -----------------------------------------------------------------------------
// 常量
// -----------------------------------------------------------------------------

/// PAGE 大小用位移表示：2^12 = 4096 字节。
/// 地址 → PFN 用位移而非除法，速度更快。
static constexpr uint32_t BUDDY_PAGE_SHIFT = 12u;

/// PAGE 字节数 = 4096，整个系统的最小分配粒度。
static constexpr uint32_t BUDDY_PAGE_SIZE  = 1u << BUDDY_PAGE_SHIFT;

/// free_area 数组长度上界（阶 0..10），最大单次分配 = 2^10 PAGE = 4 MB。
/// 实际用到几阶由 pool 大小决定（max_order 字段）。
static constexpr uint32_t BUDDY_MAX_ORDER  = 11u;

// PAGE 状态标志位（可组合，不用枚举是因为 PG_SLAB|PG_TAIL 需要同时存在）
static constexpr uint32_t PG_BUDDY = 0x01u; ///< 在 buddy 空闲链表中，可被分配
static constexpr uint32_t PG_SLAB  = 0x04u; ///< 已被 slab 层持有，buddy 不能再动
static constexpr uint32_t PG_TAIL  = 0x08u; ///< slab 复合块的尾页（与 PG_SLAB 同用）

// -----------------------------------------------------------------------------
// struct bpage —— 每个 4KB PAGE 的"管理卡片"
//
// 11 个字段在 buddy 模式和 slab 模式下含义不同，靠 flags 区分。
// 这种复用是节省内存的关键设计（一个结构体支撑三种使用模式）。
// -----------------------------------------------------------------------------
struct bpage {
    /// PAGE 当前状态：0=已分配/使用中，PG_BUDDY=空闲，PG_SLAB=slab持有，
    ///                PG_SLAB|PG_TAIL=slab尾页。
    uint32_t flags;

    /// buddy模式：块大小 = 2^order PAGE，只有头页的 order 有意义。
    /// slab模式：不被改写，由 slab_cache->page_order 表达块大小。
    uint32_t order;

    /// 引用计数。buddy_alloc 后=1；buddy_free 先 --，降到 0 才真正合并回收。
    /// 支持零拷贝：多个 subscriber 共享同一块内存时各持一个引用。
    int32_t refcount;

    /// buddy模式：free_area[order] 双向循环链表的 next/prev。
    /// slab模式：link_next 串联 partial/full 单向链表；link_prev 不用（保持NULL）。
    /// 两种模式严格互斥，复用同一对指针不会产生歧义。
    struct bpage *link_next;
    struct bpage *link_prev;

    /// 头页(无PG_TAIL)：指向拥有此 slab page 的 struct slab_cache*。
    /// 尾页(有PG_TAIL)：指向本复合块的头页 struct bpage*（O(1)头页查找关键）。
    void *slab_cache;

    /// 仅头页有意义：本 slab page 内第一个可用空闲对象的 sobj_ref* 链表头。
    /// 注意：存的是 sobj_ref*，不是对象地址，需 ref_to_addr() 换算。
    void *freelist;

    /// 已归还到 freelist 等待再次分配的对象数。alloc 取走时--，free 归还时++。
    int16_t nr_free;

    /// 本 slab page 能容纳的对象总数，初始化后不变。
    int16_t nr_total;

    /// 从未被分配过的对象数（延迟切分），初值=nr_total，每次 uninit 路径分配时--。
    int16_t nr_uninit;

    int16_t _pad; // 对齐填充
};

// -----------------------------------------------------------------------------
// struct free_area —— 单阶空闲链表
//
// sentinel 是哨兵节点（值，非指针），空链时自环，非空时 link_next 指向第一个真块。
// 哨兵模式好处：insert/delete 无需判断链表是否为空，代码简洁。
// -----------------------------------------------------------------------------
struct free_area {
    struct bpage sentinel; ///< 哨兵头节点，flags/order 字段无意义，只作链表锚点
    int32_t      count;    ///< 本阶当前空闲块数，buddy_dump/快速判空用
};

// -----------------------------------------------------------------------------
// struct buddy_zone —— 整个 buddy 分配器的根
//
// 调用者负责提供 pool 和 page_table 两块内存，分配器本身不调用 malloc。
// 这使得整套系统可以直接用于共享内存（pool 来自 mmap，page_table 在私有内存）。
// -----------------------------------------------------------------------------
struct buddy_zone {
    uint8_t       *pool;       ///< 被管理的字节数组，所有分配地址都在 [pool, pool+pool_size)
    size_t         pool_size;  ///< 总字节数，必须是 PAGE_SIZE 的整数倍
    uint32_t       nr_pages;   ///< = pool_size / PAGE_SIZE，合法 PFN 范围 [0, nr_pages)
    uint32_t       page_shift; ///< 固定 = BUDDY_PAGE_SHIFT(12)，存此处避免依赖全局宏
    uint32_t       max_order;  ///< free_area 有效范围 [0, max_order)，由 pool 大小决定

    /// 调用者提供的 bpage 数组，长度 >= nr_pages。
    /// page_table[pfn] 是 pool 中第 pfn 个 4KB PAGE 的元数据（1:1 对应）。
    struct bpage  *page_table;

    /// 核心：11 个槽位，free_area[i] 管所有大小为 2^i PAGE 的空闲块。
    struct free_area free_area[BUDDY_MAX_ORDER];
};

// -----------------------------------------------------------------------------
// 地址 <-> bpage 互转（内联，零开销）
// -----------------------------------------------------------------------------

/// bpage* → 对应 PAGE 的起始地址
static inline uint8_t *bpage_to_addr(const struct buddy_zone *zone,
                                      const struct bpage      *page) {
    uint32_t pfn = (uint32_t)(page - zone->page_table);
    return zone->pool + ((size_t)pfn << zone->page_shift);
}

/// 对象地址 → 对应的 bpage*（地址必须在 pool 范围内且页对齐）
static inline struct bpage *addr_to_bpage(const struct buddy_zone *zone,
                                           const uint8_t           *addr) {
    uint32_t pfn = (uint32_t)((addr - zone->pool) >> zone->page_shift);
    return &zone->page_table[pfn];
}

// -----------------------------------------------------------------------------
// 公开 API
// -----------------------------------------------------------------------------

/// 初始化 buddy_zone。
/// pool 必须 PAGE_SIZE 对齐，page_table 长度 >= pool_size/PAGE_SIZE。
/// 内部通过"自举初始化"：把每个 PAGE 当成一次 free，合并算法自动建树。
bool buddy_zone_init(struct buddy_zone *zone,
                     uint8_t           *pool,
                     size_t             pool_size,
                     struct bpage      *page_table);

/// 分配 2^order 个连续 PAGE。返回头页 bpage*，失败返回 nullptr。
/// 调用者用 bpage_to_addr() 得到真实地址。
/// 返回的是 bpage* 而非地址，是因为 slab 层需要直接操作 bpage 元数据。
struct bpage *buddy_alloc(struct buddy_zone *zone, uint32_t order);

/// 释放由 buddy_alloc 返回的块。先减引用计数，降到 0 才真正合并回池。
void buddy_free(struct buddy_zone *zone, struct bpage *page);

/// 打印各阶空闲块统计，供调试用。
void buddy_dump(const struct buddy_zone *zone);

} // namespace dcms
