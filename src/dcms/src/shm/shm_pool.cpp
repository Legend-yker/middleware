// 首版 stub：ShmPool 使用 malloc 模拟共享内存区域，
// 真实 shm_open + mmap 后续迭代。

#include "shm/shm_pool.h"
#include "shm/buddy_allocator.h"
#include "shm/slab_allocator.h"

#include <cstdlib>
#include <cstring>

namespace dcms {

ShmPool::ShmPool() = default;

ShmPool::~ShmPool() { destroy(); }

ShmPool &ShmPool::instance() {
    static ShmPool inst;
    return inst;
}

bool ShmPool::create(const std::string &name, std::size_t size) {
    if (region_)
        return false;

    region_ = std::malloc(size);
    if (!region_)
        return false;
    std::memset(region_, 0, size);
    poolSize_ = size;
    shmName_ = name;

    buddy_ = new BuddyAllocator();
    buddy_->init(region_, size);

    slab_ = new SlabAllocator();
    slab_->init(region_, size, 256);

    return true;
}

bool ShmPool::attach(const std::string &name) {
    // stub: 当前进程内 create 即 attach
    (void)name;
    return region_ != nullptr;
}

void ShmPool::destroy() {
    delete slab_;
    slab_ = nullptr;
    delete buddy_;
    buddy_ = nullptr;
    std::free(region_);
    region_ = nullptr;
    poolSize_ = 0;
}

void *ShmPool::alloc(std::size_t size) {
    if (!buddy_)
        return nullptr;
    return buddy_->alloc(size);
}

void ShmPool::free(void *ptr) {
    if (buddy_)
        buddy_->free(ptr);
}

} // namespace dcms
