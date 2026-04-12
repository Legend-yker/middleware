// Stub implementation: delegates to malloc/free.
// Replace with a real Buddy allocator (power-of-two splitting/merging) in a
// future iteration.

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
