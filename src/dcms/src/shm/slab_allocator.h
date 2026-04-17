#pragma once

#include "shm/buddy_allocator.h"

#include <cstdint>

// =============================================================================
// slab_allocator.h — Slab 对象缓存分配器
//
// Slab 层建立在 Buddy 层之上，把大块内存切成固定大小的对象（obj_size）。
//
// 与标准 Linux slab 最大的差异：
//   标准 slab 把"下一个空闲对象的地址"写进对象本身的头几个字节。
//   这在共享内存里有两个致命问题：
//     1. 多进程 mmap 到不同虚拟地址 → 写进去的指针在另一进程完全无效。
//     2. 消息传输中发送方写了数据 → 接收方会看到"头几字节"被链表指针污染。
//
//   本设计用 ref_array 解决：在每个进程自己的私有普通内存里维护空闲链表，
//   ref_array[pfn] 就是 pfn 对应 PAGE 上对象的链表节点。
//   空闲链表完全不接触 pool（共享内存），多进程安全。
// =============================================================================

namespace dcms {

// -----------------------------------------------------------------------------
// 常量
// -----------------------------------------------------------------------------

/// slab_ctx 最多管理的缓存数（内嵌数组，不依赖动态分配）
static constexpr int32_t  SLAB_MAX_CACHES = 8;

/// sobj_ref.flags 置位 = 对象已分配；清零 = 空闲。可检测 double-free。
static constexpr uint16_t SOBJ_BUSY = 0x0001u;

// -----------------------------------------------------------------------------
// struct sobj_ref —— 外部空闲链表节点
//
// 存在进程私有的 ref_array 中，绝对不写 pool。
// ref_array[pfn] 对应 pool 中 pfn 号 PAGE 上的那个对象。
//
// ref → ref（不是地址→地址）：
//   真实对象地址由 ref_to_addr() 计算：
//     pfn = ref - ref_array
//     addr = mem_base + pfn * PAGE_SIZE
// -----------------------------------------------------------------------------
struct sobj_ref {
    int16_t          refcount; ///< 对象级引用计数（零拷贝订阅时多人持有）
    uint16_t         flags;    ///< SOBJ_BUSY=已分配，0=空闲
    struct sobj_ref *next;     ///< 同 slab page 内下一个空闲对象的 ref（单向链表）
};

// -----------------------------------------------------------------------------
// struct slab_cache —— 固定大小对象缓存
//
// obj_size 必须是 PAGE_SIZE 的整数倍，保证每个对象起始地址页对齐，
// 使得 pfn 能唯一标识一个对象（addr_to_ref / ref_to_addr 的前提）。
// -----------------------------------------------------------------------------
struct slab_cache {
    /// partial：有空位的 slab page 链表头（nr_free>0 或 nr_uninit>0），单向链表
    struct bpage *partial;
    /// full：已全满的 slab page 链表头（nr_free=0 且 nr_uninit=0），单向链表
    struct bpage *full;

    int32_t nr_partial; ///< partial 链表节点数，O(1) 判断是否有可用 slab page
    int32_t nr_full;    ///< full 链表节点数

    /// 每个对象的字节数，必须是 PAGE_SIZE 的整数倍
    int32_t obj_size;

    /// 每个 slab page（= 2^page_order 个物理 PAGE）能容纳的对象数
    /// = (2^page_order * PAGE_SIZE) / obj_size，创建时算好，之后不变
    int16_t objs_per_page;

    /// 向 buddy 申请新 slab page 时的 order 参数
    /// slab page 总字节 = 2^page_order * PAGE_SIZE
    uint8_t page_order;

    char name[32]; ///< 调试用名称（如 "cmd_4k"、"sensor_8k"）
};

// -----------------------------------------------------------------------------
// struct slab_ctx —— slab 层全局上下文
//
// 完全不依赖动态分配：caches[] 内嵌，整个结构体可放在调用者栈或固定内存里。
// 这使得系统可以用在任何不能调用 malloc 的场景（共享内存初始化、内核态）。
// -----------------------------------------------------------------------------
struct slab_ctx {
    struct buddy_zone *zone;      ///< 底层 buddy 分配器
    uint8_t           *mem_base;  ///< = zone->pool，单独缓存避免每次间接寻址
    struct sobj_ref   *ref_array; ///< 调用者提供，长度 >= zone->nr_pages，进程私有
    uint32_t           ref_len;   ///< = zone->nr_pages，越界检查用

    /// 内嵌缓存数组，最多 SLAB_MAX_CACHES 个，按 ncaches 依次使用
    struct slab_cache caches[SLAB_MAX_CACHES];
    int32_t           ncaches; ///< 已创建的缓存数，也是下次 create 时写入的下标
};

// -----------------------------------------------------------------------------
// 内联辅助函数
// -----------------------------------------------------------------------------

/// 根据指针 ptr 找到它所属 slab page 的头页（O(1)）。
/// 若 ptr 落在尾页（PG_TAIL），slab_cache 字段存的是头页 bpage*，一步取到。
/// 这解决了多页对象（obj_size > PAGE_SIZE）中 ptr 可能对应任意 PAGE 的问题。
static inline struct bpage *get_head_page(const struct slab_ctx *ctx,
                                          const void            *ptr) {
    uint32_t      pfn  = (uint32_t)(((const uint8_t *)ptr - ctx->mem_base)
                                    >> BUDDY_PAGE_SHIFT);
    struct bpage *page = &ctx->zone->page_table[pfn];
    // 尾页：slab_cache 字段被复用为头页 bpage* 指针
    if (page->flags & PG_TAIL)
        return static_cast<struct bpage *>(page->slab_cache);
    return page;
}

/// sobj_ref* → 对象起始地址
/// pfn = ref 在 ref_array 中的下标 = 对象首页的 PFN
static inline void *ref_to_addr(const struct slab_ctx  *ctx,
                                 const struct sobj_ref  *ref) {
    uint32_t pfn = (uint32_t)(ref - ctx->ref_array);
    return ctx->mem_base + ((size_t)pfn << BUDDY_PAGE_SHIFT);
}

/// 对象起始地址 → sobj_ref*
static inline struct sobj_ref *addr_to_ref(const struct slab_ctx *ctx,
                                            const void            *addr) {
    uint32_t pfn = (uint32_t)(((const uint8_t *)addr - ctx->mem_base)
                               >> BUDDY_PAGE_SHIFT);
    return &ctx->ref_array[pfn];
}

// -----------------------------------------------------------------------------
// 公开 API
// -----------------------------------------------------------------------------

/// 初始化 slab_ctx。
/// ref_array 由调用者分配，长度 >= zone->nr_pages，存在进程私有普通内存里。
bool slab_ctx_init(struct slab_ctx   *ctx,
                   struct buddy_zone *zone,
                   struct sobj_ref   *ref_array,
                   uint32_t           ref_len);

/// 创建一个新的对象缓存。
/// obj_size 必须是 PAGE_SIZE 的整数倍；page_order 决定每次向 buddy 申请的块大小。
/// 返回 ctx->caches[] 内的指针；缓存已满（ncaches==SLAB_MAX_CACHES）时返回 nullptr。
struct slab_cache *slab_cache_create(struct slab_ctx *ctx,
                                     const char      *name,
                                     int32_t          obj_size,
                                     uint8_t          page_order);

/// 从 cache 分配一个对象，返回对象起始地址；内存不足时返回 nullptr。
/// 优先走 freelist 路径（已归还对象），其次走 uninit 路径（延迟切分）。
void *slab_alloc(struct slab_ctx *ctx, struct slab_cache *cache);

/// 释放对象。只需要 ptr，无需指定 cache。
/// 全链追溯：ptr → pfn → bpage.slab_cache → struct slab_cache*，自动完成。
/// 当 slab page 完全空闲时自动归还给 buddy（buddy_free 内部再合并）。
void slab_free(struct slab_ctx *ctx, void *ptr);

/// 打印各 cache 的 partial/full 统计，供调试用。
void slab_dump(const struct slab_ctx *ctx);

} // namespace dcms
