// =============================================================================
// slab_allocator.cpp — Slab 对象缓存分配器核心实现
//
// slab_alloc 的两条取对象路径：
//   路径一（freelist）：从已归还对象链表弹出头节点，O(1)。
//   路径二（uninit）：从未初始化区按序切出下一个对象（延迟切分），O(1)。
//
// slab_free 的全空判断：
//   if (nr_free + nr_uninit == nr_total) → slab page 完全空闲，归还给 buddy。
//
// 三个计数不变式：正在使用 = nr_total - nr_free - nr_uninit。
// =============================================================================

#include "shm/slab_allocator.h"

#include <cassert>
#include <cstdio>
#include <cstring>

namespace dcms {

// =============================================================================
// 内部：单向链表辅助（partial / full 链表用 link_next 串联）
// =============================================================================

/// 从单向链表 *list_head 中移除 target 节点（O(n)，slab 粒度下可接受）
static void slab_list_remove(struct bpage **list_head, int32_t *count,
                              struct bpage *target) {
    struct bpage **pp = list_head;
    while (*pp && *pp != target)
        pp = &(*pp)->link_next;
    if (*pp == target) {
        *pp = target->link_next;
        target->link_next = nullptr;
        (*count)--;
    }
}

// =============================================================================
// 内部：slab_page_setup —— 配置一个刚从 buddy 取来的 slab page
//
// 头页：PG_SLAB，slab_cache 指向所属 cache，初始化计数字段。
// 尾页（非头页）：PG_SLAB|PG_TAIL，slab_cache 指向头页 bpage*（O(1) 头页查找）。
// =============================================================================
static void slab_page_setup(struct buddy_zone *zone,
                             struct bpage      *slab_head,
                             struct slab_cache *cache) {
    uint32_t head_pfn   = (uint32_t)(slab_head - zone->page_table);
    uint32_t slab_pages = 1u << cache->page_order; // 整个 slab page 占几个物理 PAGE

    // 配置头页
    slab_head->flags      = PG_SLAB;
    slab_head->slab_cache = cache;        // 头页：slab_cache 存的是 struct slab_cache*
    slab_head->freelist   = nullptr;
    slab_head->nr_free    = 0;
    slab_head->nr_total   = cache->objs_per_page;
    slab_head->nr_uninit  = cache->objs_per_page; // 所有对象一开始都未初始化（延迟切分）
    slab_head->link_next  = nullptr;

    // 配置所有尾页（包括多页对象中每个对象的非首页，也包括多对象 slab 中每个对象的首页）
    // 统一策略：slab 内所有非头页 → PG_SLAB|PG_TAIL，slab_cache 指向头页
    for (uint32_t i = 1u; i < slab_pages; i++) {
        struct bpage *tail = &zone->page_table[head_pfn + i];
        tail->flags      = PG_SLAB | PG_TAIL;
        tail->slab_cache = slab_head; // 尾页：slab_cache 被复用为头页 bpage* 指针
        tail->link_next  = nullptr;
        tail->link_prev  = nullptr;
    }
}

// =============================================================================
// 公开 API 实现
// =============================================================================

bool slab_ctx_init(struct slab_ctx   *ctx,
                   struct buddy_zone *zone,
                   struct sobj_ref   *ref_array,
                   uint32_t           ref_len) {
    if (!ctx || !zone || !ref_array) return false;
    if (ref_len < zone->nr_pages)    return false;

    ctx->zone      = zone;
    ctx->mem_base  = zone->pool;
    ctx->ref_array = ref_array;
    ctx->ref_len   = ref_len;
    ctx->ncaches   = 0;

    // 清零所有缓存槽位
    std::memset(ctx->caches, 0, sizeof(ctx->caches));

    // 初始化 ref_array：所有引用节点清零（flags=0=空闲，next=nullptr）
    std::memset(ref_array, 0, sizeof(struct sobj_ref) * ref_len);

    return true;
}

struct slab_cache *slab_cache_create(struct slab_ctx *ctx,
                                     const char      *name,
                                     int32_t          obj_size,
                                     uint8_t          page_order) {
    if (!ctx || !name) return nullptr;
    if (ctx->ncaches >= SLAB_MAX_CACHES) return nullptr; // 最多 8 个缓存

    // obj_size 必须是 PAGE_SIZE 整数倍：ref_array 以 PAGE 粒度索引，
    // 否则无法用单一 pfn 唯一标识对象起始位置，addr_to_ref/ref_to_addr 会出错。
    if (obj_size <= 0 || (obj_size & (int32_t)(BUDDY_PAGE_SIZE - 1u)) != 0)
        return nullptr;

    // 计算每个 slab page 能容纳的对象数
    size_t slab_bytes    = (size_t)(1u << page_order) * BUDDY_PAGE_SIZE;
    int16_t objs_per_page = (int16_t)(slab_bytes / (size_t)obj_size);
    if (objs_per_page <= 0) return nullptr; // obj_size 比整个 slab page 还大

    struct slab_cache *cache = &ctx->caches[ctx->ncaches++];
    cache->partial      = nullptr;
    cache->full         = nullptr;
    cache->nr_partial   = 0;
    cache->nr_full      = 0;
    cache->obj_size     = obj_size;
    cache->objs_per_page = objs_per_page;
    cache->page_order   = page_order;
    std::strncpy(cache->name, name, sizeof(cache->name) - 1u);
    cache->name[sizeof(cache->name) - 1u] = '\0';

    return cache;
}

void *slab_alloc(struct slab_ctx *ctx, struct slab_cache *cache) {
    if (!ctx || !cache) return nullptr;

    // 若 partial 链表为空，向 buddy 申请一个新的 slab page
    if (cache->nr_partial == 0) {
        struct bpage *slab = buddy_alloc(ctx->zone, cache->page_order);
        if (!slab) return nullptr; // buddy 内存不足

        slab_page_setup(ctx->zone, slab, cache);

        // 加入 partial 链表头部（最新的 slab page 最先被使用）
        slab->link_next  = cache->partial;
        cache->partial   = slab;
        cache->nr_partial++;
    }

    // 从 partial 链表头取 slab page（始终操作链表头，O(1)）
    struct bpage *head     = cache->partial;
    uint32_t      head_pfn = (uint32_t)(head - ctx->zone->page_table);
    void         *obj      = nullptr;

    if (head->nr_free > 0) {
        // -----------------------------------------------------------------
        // 路径一：freelist（已归还但尚未再分配的对象）
        // freelist 存 sobj_ref*，通过 ref_to_addr() 换算回真实地址
        // -----------------------------------------------------------------
        struct sobj_ref *ref = static_cast<struct sobj_ref *>(head->freelist);
        head->freelist = ref->next; // 弹出链表头
        head->nr_free--;
        ref->flags = SOBJ_BUSY;
        obj = ref_to_addr(ctx, ref);

    } else {
        // -----------------------------------------------------------------
        // 路径二：uninit 区域（从未分配过的对象，按序切出）
        // 延迟切分：slab page 刚创建时不预先初始化所有对象，
        // 只在首次分配时设置 ref_array 标志，减少一次性初始化开销。
        // -----------------------------------------------------------------
        int16_t  obj_idx      = head->nr_total - head->nr_uninit; // 当前切第几个（0-based）
        uint32_t pages_per_obj = (uint32_t)(cache->obj_size >> BUDDY_PAGE_SHIFT);
        uint32_t obj_pfn      = head_pfn + (uint32_t)obj_idx * pages_per_obj;

        obj = ctx->mem_base + ((size_t)obj_pfn << BUDDY_PAGE_SHIFT);
        head->nr_uninit--;
        ctx->ref_array[obj_pfn].flags = SOBJ_BUSY;
    }

    // 若 slab page 已全满（nr_free=0 且 nr_uninit=0），从 partial 移到 full
    if (head->nr_free == 0 && head->nr_uninit == 0) {
        cache->partial = head->link_next; // 摘除链表头（O(1)，因为我们总取头部）
        cache->nr_partial--;
        head->link_next = cache->full;    // 插入 full 链表头
        cache->full     = head;
        cache->nr_full++;
    }

    return obj;
}

void slab_free(struct slab_ctx *ctx, void *ptr) {
    if (!ctx || !ptr) return;

    // 步骤1：通过 ptr → pfn → bpage 找到 slab page 头页（O(1)）
    struct bpage      *head  = get_head_page(ctx, ptr);
    struct slab_cache *cache = static_cast<struct slab_cache *>(head->slab_cache);

    // 步骤2：找到对应 sobj_ref，清除 SOBJ_BUSY 标志
    struct sobj_ref *ref = addr_to_ref(ctx, ptr);
    ref->flags &= ~SOBJ_BUSY;

    // 步骤3：记录释放前 slab page 是否全满（用于决定是否从 full 移到 partial）
    // 全满条件：nr_free=0 且 nr_uninit=0（即所有对象都在使用中）
    bool was_full = (head->nr_free == 0 && head->nr_uninit == 0);

    // 步骤4：将 ref 头插入 freelist
    ref->next      = static_cast<struct sobj_ref *>(head->freelist);
    head->freelist = ref;
    head->nr_free++;

    // 步骤5：若之前全满，将 slab page 从 full 移到 partial
    if (was_full) {
        slab_list_remove(&cache->full, &cache->nr_full, head);
        head->link_next = cache->partial;
        cache->partial  = head;
        cache->nr_partial++;
    }

    // 步骤6：全空检查
    // 不变式：正在使用 = nr_total - nr_free - nr_uninit
    // 全空 ⟺ 正在使用=0 ⟺ nr_free + nr_uninit == nr_total
    //
    // 注意：nr_uninit 不会因 free 增加——uninit 区的对象一旦切出就进 freelist，
    // 不会回去 uninit。因此此公式在所有路径下都正确。
    if ((int32_t)(head->nr_free + head->nr_uninit) == (int32_t)head->nr_total) {
        // 从 partial 链表移除
        slab_list_remove(&cache->partial, &cache->nr_partial, head);

        // 清除 slab page 内所有物理 PAGE 的 PG_SLAB / PG_TAIL 标志
        uint32_t head_pfn   = (uint32_t)(head - ctx->zone->page_table);
        uint32_t slab_pages = 1u << cache->page_order;
        for (uint32_t i = 0u; i < slab_pages; i++) {
            ctx->zone->page_table[head_pfn + i].flags &= ~(PG_SLAB | PG_TAIL);
        }

        // 重置头页元数据
        head->freelist   = nullptr;
        head->nr_free    = 0;
        head->slab_cache = nullptr;

        // 归还给 buddy（buddy_free 内部的合并逻辑会自动合并相邻空闲块）
        buddy_free(ctx->zone, head);
    }
}

void slab_dump(const struct slab_ctx *ctx) {
    if (!ctx) return;
    std::printf("[slab_dump] ncaches=%d\n", ctx->ncaches);
    for (int32_t i = 0; i < ctx->ncaches; i++) {
        const struct slab_cache *c = &ctx->caches[i];
        std::printf("  cache[%d] %-16s  obj_size=%d  page_order=%u"
                    "  objs/slab=%d  partial=%d  full=%d\n",
                    i, c->name, c->obj_size, (unsigned)c->page_order,
                    (int)c->objs_per_page, c->nr_partial, c->nr_full);
    }
}

} // namespace dcms
