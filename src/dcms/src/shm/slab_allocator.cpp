// 首版 stub：直接用 malloc/free，Slab 缓存池后续迭代填充。

#include "shm/slab_allocator.h"

#include <cstdlib>

namespace dcms {

SlabAllocator::SlabAllocator() = default;
SlabAllocator::~SlabAllocator() = default;

bool SlabAllocator::init(void *region, std::size_t totalSize, std::size_t objectSize) {
    region_ = region;
    totalSize_ = totalSize;
    objectSize_ = objectSize;
    return region_ != nullptr && totalSize_ > 0 && objectSize_ > 0;
}

void *SlabAllocator::alloc() {
    return std::malloc(objectSize_ > 0 ? objectSize_ : 1);
}

void SlabAllocator::free(void *ptr) { std::free(ptr); }

} // namespace dcms
