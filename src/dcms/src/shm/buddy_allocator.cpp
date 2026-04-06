// 首版 stub：直接用 malloc/free，Buddy 算法后续迭代填充。

#include "shm/buddy_allocator.h"

#include <cstdlib>

namespace dcms {

BuddyAllocator::BuddyAllocator() = default;
BuddyAllocator::~BuddyAllocator() = default;

bool BuddyAllocator::init(void *region, std::size_t totalSize) {
    region_ = region;
    totalSize_ = totalSize;
    return region_ != nullptr && totalSize_ > 0;
}

void *BuddyAllocator::alloc(std::size_t size) {
    if (size == 0)
        return nullptr;
    return std::malloc(size);
}

void BuddyAllocator::free(void *ptr) { std::free(ptr); }

} // namespace dcms
