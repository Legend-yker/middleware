// =============================================================================
// buddy_allocator.cpp — Buddy 分配器核心实现
//
// 核心算法：
//   分配(buddy_alloc)：从 free_area[order] 向上找非空阶，取块后 expand 折回低阶。
//   释放(buddy_free)：引用计数降到 0 后 coalesce_and_add 合并相邻伙伴块。
//   初始化(buddy_zone_init)：自举——把每页当成一次 free，合并算法自动建正确初态。
// =============================================================================

#include "shm/buddy_allocator.h"

#include <cassert>
#include <cstdio>
#include <cstring>

namespace dcms {

// =============================================================================
// 内部：双向循环链表操作（哨兵模式）
// =============================================================================

/// 初始化 free_area：sentinel 自环 = 空链表
static void list_init(struct free_area *area) {
    area->sentinel.link_next = &area->sentinel;
    area->sentinel.link_prev = &area->sentinel;
    area->count = 0;
}

/// 将 page 插入链表头（sentinel 之后）
static void list_add(struct free_area *area, struct bpage *page) {
    struct bpage *next      = area->sentinel.link_next;
    page->link_next         = next;
    page->link_prev         = &area->sentinel;
    area->sentinel.link_next = page;
    next->link_prev          = page;
    area->count++;
}

/// 将 page 从链表中摘除（O(1)，因为是双向链表）
static void list_del(struct free_area *area, struct bpage *page) {
    page->link_prev->link_next = page->link_next;
    page->link_next->link_prev = page->link_prev;
    page->link_next = nullptr;
    page->link_prev = nullptr;
    area->count--;
}

/// 取出链表第一个真实块（sentinel 之后），链表空时返回 nullptr
static struct bpage *list_pop(struct free_area *area) {
    if (area->count == 0) return nullptr;
    struct bpage *page = area->sentinel.link_next;
    list_del(area, page);
    return page;
}

// =============================================================================
// 内部：coalesce_and_add —— 合并伙伴并插入空闲链表
//
// 这是整个 buddy 系统的核心逻辑，buddy_free 和 buddy_zone_init 都调用它。
//
// 两个关键位运算（order=1 二进制示例）：
//   buddy_pfn  = pfn XOR (1 << order)   // 翻转第 order 位 → 找到伙伴
//   merged_pfn = pfn AND buddy_pfn      // 取公共前缀    → 合并后的头位置
//
// 对称性：无论从哪侧伙伴触发，merged_pfn 结果相同，不需要 if(pfn < buddy_pfn)。
// =============================================================================
static void coalesce_and_add(struct buddy_zone *zone, uint32_t pfn, uint32_t order) {
    while (order < zone->max_order - 1u) {
        // 找伙伴：翻转第 order 位
        uint32_t buddy_pfn  = pfn ^ (1u << order);
        // 合并头：取两者公共二进制前缀（即较小的那个）
        uint32_t merged_pfn = pfn & buddy_pfn;

        // 伙伴必须满足三个条件才能合并：在范围内、是空闲块、阶数相同
        if (buddy_pfn >= zone->nr_pages)
            break;
        struct bpage *buddy = &zone->page_table[buddy_pfn];
        if (!(buddy->flags & PG_BUDDY) || buddy->order != order)
            break;

        // 将伙伴从当前阶链表摘除，升阶合并
        list_del(&zone->free_area[order], buddy);
        buddy->flags &= ~PG_BUDDY;

        pfn = merged_pfn;
        order++;
    }

    // 把（可能已升阶的）合并块标记为空闲并插入对应链表
    struct bpage *head = &zone->page_table[pfn];
    head->flags    = PG_BUDDY;
    head->order    = order;
    head->refcount = 0;
    list_add(&zone->free_area[order], head);
}

// =============================================================================
// 内部：expand —— 将过大的块拆分到目标阶
//
// 从 cur_order 逐步对折到 target_order，每次把后半块放入对应阶的空闲链表。
//
// 示例：cur=3, target=1，块 [0..7]（8PAGE）：
//   cur=3→2: 后 4PAGE [4..7] → free_area[2]，剩 [0..3]
//   cur=2→1: 后 2PAGE [2..3] → free_area[1]，剩 [0..1]
//   返回 [0..1]（2PAGE，order=1 的块）✓
// =============================================================================
static void expand(struct buddy_zone *zone, struct bpage *page,
                   uint32_t cur_order, uint32_t target_order) {
    uint32_t pfn = (uint32_t)(page - zone->page_table);

    while (cur_order > target_order) {
        cur_order--;
        // 后半块从 pfn + 2^cur_order 开始
        uint32_t      half_pfn = pfn + (1u << cur_order);
        struct bpage *half     = &zone->page_table[half_pfn];
        half->flags    = PG_BUDDY;
        half->order    = cur_order;
        half->refcount = 0;
        list_add(&zone->free_area[cur_order], half);
    }
    // 此时 page 代表从 pfn 开始的 2^target_order 页块
}

// =============================================================================
// 公开 API 实现
// =============================================================================

bool buddy_zone_init(struct buddy_zone *zone,
                     uint8_t           *pool,
                     size_t             pool_size,
                     struct bpage      *page_table) {
    if (!zone || !pool || !page_table)           return false;
    if (pool_size == 0)                          return false;
    if (pool_size & (BUDDY_PAGE_SIZE - 1u))      return false; // 必须是 PAGE 整数倍
    if ((uintptr_t)pool & (BUDDY_PAGE_SIZE - 1u)) return false; // pool 必须页对齐

    zone->pool       = pool;
    zone->pool_size  = pool_size;
    zone->nr_pages   = (uint32_t)(pool_size >> BUDDY_PAGE_SHIFT);
    zone->page_shift = BUDDY_PAGE_SHIFT;
    zone->page_table = page_table;

    // 计算 max_order = floor(log2(nr_pages)) + 1，上限 BUDDY_MAX_ORDER
    // max_order 是 free_area 有效范围的排外上界：合法阶 [0, max_order)
    // 例：128PAGE → max_order=8，最大可用阶=7，最大单次分配=2^7=128PAGE=512KB
    {
        uint32_t mo = 0u;
        uint32_t n  = zone->nr_pages;
        while (n > 1u) { n >>= 1u; mo++; }
        zone->max_order = (mo + 1u <= BUDDY_MAX_ORDER) ? mo + 1u : BUDDY_MAX_ORDER;
    }

    // 初始化所有 free_area：sentinel 自环（哨兵模式）
    for (uint32_t i = 0u; i < BUDDY_MAX_ORDER; i++)
        list_init(&zone->free_area[i]);

    // 初始化所有 bpage：refcount=1 模拟"已分配"状态，flags=0（非空闲）
    for (uint32_t pfn = 0u; pfn < zone->nr_pages; pfn++) {
        struct bpage *p = &page_table[pfn];
        p->flags      = 0u;
        p->order      = 0u;
        p->refcount   = 1;   // 标记"已分配"，后续 coalesce_and_add 不依赖此值
        p->link_next  = nullptr;
        p->link_prev  = nullptr;
        p->slab_cache = nullptr;
        p->freelist   = nullptr;
        p->nr_free    = 0;
        p->nr_total   = 0;
        p->nr_uninit  = 0;
        p->_pad       = 0;
    }

    // 自举初始化：从最大阶向最小阶贪心铺设空闲块。
    //
    // 不能简单地"对每一页调用 coalesce_and_add(order=0)"：已经并进大块的页
    // 会被再次插入链表（伙伴阶数不匹配时 break 后仍执行 list_add），同一段
    // 内存在多个阶链表里重复计入，导致双重记账 —— 后续会分配出互相重叠的
    // 块，写坏 ShmBufHeader / free list，表现为"内存不足"或随机段错误。
    //
    // 贪心铺设天然保证每个块对齐到自身大小，且每页只归属一个块。
    // 例：nr_pages=7，max_order=3 → [0..3]阶2 + [4..5]阶1 + [6]阶0。
    {
        uint32_t used = 0u;
        for (int32_t order = static_cast<int32_t>(zone->max_order) - 1;
             order >= 0; --order) {
            const uint32_t block_pages = 1u << order;
            while (used + block_pages <= zone->nr_pages) {
                struct bpage *head = &page_table[used];
                head->flags    = PG_BUDDY;
                head->order    = static_cast<uint8_t>(order);
                head->refcount = 0;
                list_add(&zone->free_area[order], head);
                used += block_pages;
            }
        }
    }

    return true;
}

struct bpage *buddy_alloc(struct buddy_zone *zone, uint32_t order) {
    if (!zone || order >= zone->max_order) return nullptr;

    // 从 free_area[order] 向上找第一个非空的阶
    uint32_t cur = order;
    while (cur < zone->max_order && zone->free_area[cur].count == 0)
        cur++;
    if (cur >= zone->max_order) return nullptr; // 内存不足

    // 从 free_area[cur] 取出第一个块，清除 PG_BUDDY 标志
    struct bpage *page = list_pop(&zone->free_area[cur]);
    page->flags &= ~PG_BUDDY;

    // 若取到的块比请求的大，将多余部分折回低阶（expand）
    expand(zone, page, cur, order);

    // 标记为已分配
    page->order    = order;
    page->refcount = 1;
    // link_next/link_prev 已在 list_pop → list_del 中置 nullptr

    return page;
}

void buddy_free(struct buddy_zone *zone, struct bpage *page) {
    if (!zone || !page) return;

    // 减引用计数；仍有其他 subscriber 持有引用时直接返回（零拷贝支持）
    page->refcount--;
    if (page->refcount > 0) return;

    // 引用归零：合并并归还到空闲链表
    uint32_t pfn   = (uint32_t)(page - zone->page_table);
    uint32_t order = page->order;
    coalesce_and_add(zone, pfn, order);
}

void buddy_dump(const struct buddy_zone *zone) {
    if (!zone) return;
    std::printf("[buddy_dump] pool=%p  size=%zu B  nr_pages=%u  max_order=%u\n",
                static_cast<const void *>(zone->pool),
                zone->pool_size, zone->nr_pages, zone->max_order);
    for (uint32_t i = 0u; i < zone->max_order; i++) {
        int32_t cnt = zone->free_area[i].count;
        if (cnt > 0) {
            std::printf("  order[%2u]: %d block(s), each %zu KB\n",
                        i, cnt,
                        (static_cast<size_t>(1u) << i) * BUDDY_PAGE_SIZE / 1024u);
        }
    }
}

} // namespace dcms
