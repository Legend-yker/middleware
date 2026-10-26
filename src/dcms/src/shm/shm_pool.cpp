// =============================================================================
// shm_pool.cpp — 共享内存池顶层封装实现
//
// 关键变化（对比基础版本）：
//   1. region_ 改用 shm_open + ftruncate + mmap，真正跨进程共享。
//   2. ref_array 大小改为 pool_size / min_obj_size（支持 64B 小对象）。
//   3. 注册 7 个 slab cache：64B、128B、256B、512B、1KB、2KB、4KB。
//   4. alloc() 改为"最佳适配"（找 >= obj_size 的最小 cache），而非精确匹配。
//   5. 新增 alloc_handle / free_handle / handle_to_raw / handle_payload /
//      ptr_to_handle / msg_acquire_handle / msg_recycle_handle。
//   6. init_metadata 抽出共享逻辑，create/attach 都调它。
// =============================================================================

#include "shm/shm_pool.h"

#include <atomic>
#include <cstdlib>
#include <cstdio>
#include <cstring>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace dcms {

ShmPool::ShmPool()  = default;
ShmPool::~ShmPool() { destroy(); }

ShmPool &ShmPool::instance() {
    static ShmPool inst;
    return inst;
}

// =============================================================================
// init_metadata — 给定一块已 mmap 的 mem，初始化 buddy + slab 层
// =============================================================================
bool ShmPool::init_metadata(void *mem, std::size_t size, uint32_t min_obj_size) {
    if (!mem || size == 0 || (size & (BUDDY_PAGE_SIZE - 1u)) != 0) return false;
    if (min_obj_size == 0 || (min_obj_size & (min_obj_size - 1u)) != 0) return false;

    uint32_t nr_pages = (uint32_t)(size >> BUDDY_PAGE_SHIFT);

    // page_table：进程私有
    auto *ptable = static_cast<struct bpage *>(
        std::malloc(sizeof(struct bpage) * nr_pages));
    if (!ptable) return false;
    std::memset(ptable, 0, sizeof(struct bpage) * nr_pages);

    // ref_array：索引粒度 = min_obj_size，长度 = size / min_obj_size
    uint32_t ref_len = (uint32_t)(size / min_obj_size);
    auto *rarray = static_cast<struct sobj_ref *>(
        std::malloc(sizeof(struct sobj_ref) * ref_len));
    if (!rarray) { std::free(ptable); return false; }

    // buddy zone
    if (!buddy_zone_init(&zone_, static_cast<uint8_t *>(mem), size, ptable)) {
        std::free(rarray); std::free(ptable);
        return false;
    }

    // slab ctx
    if (!slab_ctx_init(&slab_ctx_, &zone_, rarray, ref_len, min_obj_size)) {
        std::free(rarray); std::free(ptable);
        return false;
    }

    // 7 个 slab cache：64B ~ 4KB（覆盖公司文档要求的小对象范围）
    // 全部使用 page_order=0（1 个 4KB 物理页 per slab page）
    // objs/slab: 64B→64, 128B→32, 256B→16, 512B→8, 1KB→4, 2KB→2, 4KB→1
    slab_cache_create(&slab_ctx_, "obj_64",   64,   0);
    slab_cache_create(&slab_ctx_, "obj_128",  128,  0);
    slab_cache_create(&slab_ctx_, "obj_256",  256,  0);
    slab_cache_create(&slab_ctx_, "obj_512",  512,  0);
    slab_cache_create(&slab_ctx_, "obj_1k",   1024, 0);
    slab_cache_create(&slab_ctx_, "obj_2k",   2048, 0);
    slab_cache_create(&slab_ctx_, "obj_4k",   4096, 0);
    // 大对象（> 4KB）回落到 buddy 直接分配

    page_table_ = ptable;
    ref_array_  = rarray;
    return true;
}

// =============================================================================
// create — 首进程创建命名 SHM（若已存在则直接 attach）
// =============================================================================
bool ShmPool::create(const std::string &name, std::size_t size,
                     uint32_t min_obj_size) {
    if (region_) return false; // 已初始化

    if (size == 0 || (size & (BUDDY_PAGE_SIZE - 1u)) != 0) return false;

    // O_CREAT | O_RDWR：不存在则创建，已存在则打开（幂等）
    int fd = ::shm_open(name.c_str(), O_CREAT | O_RDWR, 0666);
    if (fd < 0) {
        std::perror("[ShmPool::create] shm_open");
        return false;
    }

    // 调整大小（若刚创建 size=0，ftruncate 设置）
    struct stat st{};
    ::fstat(fd, &st);
    if (st.st_size == 0) {
        if (::ftruncate(fd, static_cast<off_t>(size)) < 0) {
            std::perror("[ShmPool::create] ftruncate");
            ::close(fd);
            ::shm_unlink(name.c_str());
            return false;
        }
    } else {
        size = static_cast<std::size_t>(st.st_size); // 已存在：用实际大小
    }

    void *mem = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    ::close(fd);
    if (mem == MAP_FAILED) {
        std::perror("[ShmPool::create] mmap");
        return false;
    }

    if (!init_metadata(mem, size, min_obj_size)) {
        ::munmap(mem, size);
        return false;
    }

    region_   = mem;
    poolSize_ = size;
    shmName_  = name;
    is_owner_ = true;
    return true;
}

// =============================================================================
// attach — 后续进程附加（page_table/ref_array 进程私有重建，不写 SHM 本身）
// =============================================================================
bool ShmPool::attach(const std::string &name) {
    if (region_) return true; // 已挂载

    int fd = ::shm_open(name.c_str(), O_RDWR, 0);
    if (fd < 0) {
        std::perror("[ShmPool::attach] shm_open");
        return false;
    }

    struct stat st{};
    if (::fstat(fd, &st) < 0 || st.st_size == 0) {
        ::close(fd);
        return false;
    }

    std::size_t size = static_cast<std::size_t>(st.st_size);
    void *mem = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    ::close(fd);
    if (mem == MAP_FAILED) {
        std::perror("[ShmPool::attach] mmap");
        return false;
    }

    // 注意：attach 进程重建 page_table/ref_array 为进程私有空副本。
    // 这意味着 attach 进程【不能】调用 alloc/free（它们会修改进程私有的 page_table，
    // 与 create 进程的视图不一致）。attach 进程只能通过 ShmHandle 读写数据，
    // 以及通过 msg_acquire_handle / msg_recycle_handle 操作 ShmBufHeader.refcount
    // （refcount 存在共享内存里，是真正的跨进程原子变量）。
    if (!init_metadata(mem, size, SLAB_MIN_OBJ_SIZE)) {
        ::munmap(mem, size);
        return false;
    }

    region_   = mem;
    poolSize_ = size;
    shmName_  = name;
    is_owner_ = false;
    return true;
}

// =============================================================================
// destroy
// =============================================================================
void ShmPool::destroy() {
    std::free(ref_array_);  ref_array_  = nullptr;
    std::free(page_table_); page_table_ = nullptr;

    if (region_) {
        ::munmap(region_, poolSize_);
        if (is_owner_ && !shmName_.empty())
            ::shm_unlink(shmName_.c_str());
        region_   = nullptr;
        poolSize_ = 0;
    }
    is_owner_ = false;
    std::memset(&zone_,     0, sizeof(zone_));
    std::memset(&slab_ctx_, 0, sizeof(slab_ctx_));
}

// =============================================================================
// alloc — 最佳适配（找最小 cache >= obj_size，否则回落 buddy）
// =============================================================================
void *ShmPool::alloc(std::size_t obj_size) {
    if (!region_) return nullptr;

    // 找 obj_size >= requested 的最小 slab cache
    struct slab_cache *best = nullptr;
    for (int32_t i = 0; i < slab_ctx_.ncaches; i++) {
        struct slab_cache *c = &slab_ctx_.caches[i];
        if (c->obj_size >= (int32_t)obj_size) {
            if (!best || c->obj_size < best->obj_size)
                best = c;
        }
    }
    if (best) return slab_alloc(&slab_ctx_, best);

    // 大对象：buddy 直接分配（向上对齐到 PAGE_SIZE 整数倍）
    if (obj_size == 0) return nullptr;
    uint32_t order = 0;
    size_t   block = BUDDY_PAGE_SIZE;
    while (block < obj_size && order + 1u < zone_.max_order) {
        order++;
        block <<= 1u;
    }
    // 请求超过 pool 支持的最大块时必须失败：绝不能"少给内存"，
    // 否则调用方会写穿块边界、破坏相邻块的 ShmBufHeader。
    if (block < obj_size || order >= zone_.max_order) return nullptr;

    struct bpage *page = buddy_alloc(&zone_, order);
    if (!page) return nullptr;
    return bpage_to_addr(&zone_, page);
}

// =============================================================================
// free
// =============================================================================
void ShmPool::free(void *ptr) {
    if (!region_ || !ptr) return;

    struct bpage *page = addr_to_bpage(&zone_, static_cast<uint8_t *>(ptr));
    if (page->flags & PG_SLAB)
        slab_free(&slab_ctx_, ptr);
    else
        buddy_free(&zone_, page);
}

// =============================================================================
// ShmHandle API
// =============================================================================

ShmHandle ShmPool::alloc_handle(std::size_t user_size) {
    const std::size_t total = sizeof(ShmBufHeader) + user_size;
    void *raw = alloc(total);
    if (!raw) return ShmHandle::invalid();

    auto *hdr = static_cast<ShmBufHeader *>(raw);
    std::memset(raw, 0, sizeof(ShmBufHeader));
    new (&hdr->refcount) std::atomic<std::int32_t>(1);
    hdr->buf_size = static_cast<std::uint32_t>(total);

    uint64_t offset = static_cast<uint8_t *>(raw) -
                      static_cast<uint8_t *>(region_);
    return ShmHandle{offset, hdr->buf_size, 0u};
}

void ShmPool::free_handle(const ShmHandle &h) {
    if (!h.valid() || !region_) return;
    free(handle_to_raw(h));
}

void *ShmPool::handle_to_raw(const ShmHandle &h) const {
    if (!h.valid() || !region_) return nullptr;
    return static_cast<uint8_t *>(region_) + h.offset;
}

void *ShmPool::handle_payload(const ShmHandle &h) const {
    void *raw = handle_to_raw(h);
    if (!raw) return nullptr;
    return static_cast<uint8_t *>(raw) + sizeof(ShmBufHeader);
}

ShmHandle ShmPool::ptr_to_handle(const void *raw_ptr) const {
    if (!raw_ptr || !region_) return ShmHandle::invalid();
    const auto *raw = static_cast<const uint8_t *>(raw_ptr);
    const auto *base = static_cast<const uint8_t *>(region_);
    if (raw < base || raw >= base + poolSize_) return ShmHandle::invalid();

    uint64_t offset = static_cast<uint64_t>(raw - base);
    auto *hdr = static_cast<const ShmBufHeader *>(raw_ptr);
    return ShmHandle{offset, hdr->buf_size, 0u};
}

void ShmPool::msg_acquire_handle(const ShmHandle &h) {
    void *raw = handle_to_raw(h);
    if (!raw) return;
    static_cast<ShmBufHeader *>(raw)->refcount.fetch_add(1,
        std::memory_order_relaxed);
}

void ShmPool::msg_recycle_handle(const ShmHandle &h) {
    void *raw = handle_to_raw(h);
    if (!raw) return;

    auto *hdr = static_cast<ShmBufHeader *>(raw);
    std::int32_t prev = hdr->refcount.fetch_sub(1, std::memory_order_release);

    if (prev <= 1) {
        std::atomic_thread_fence(std::memory_order_acquire);
        if (is_owner_)
            free(raw);
        // attach 进程：refcount 已降到 0，等待 owner 进程的 Watchdog 扫描回收
    }
}

// =============================================================================
// dump
// =============================================================================
void ShmPool::dump() const {
    buddy_dump(&zone_);
    slab_dump(&slab_ctx_);
}

} // namespace dcms
