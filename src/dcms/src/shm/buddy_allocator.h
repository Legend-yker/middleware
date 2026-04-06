#pragma once

#include "dcms_export.h"

#include <cstddef>

namespace dcms {

class DCMS_API BuddyAllocator {
public:
    BuddyAllocator();
    ~BuddyAllocator();

    BuddyAllocator(const BuddyAllocator &) = delete;
    BuddyAllocator &operator=(const BuddyAllocator &) = delete;

    bool init(void *region, std::size_t totalSize);

    void *alloc(std::size_t size);
    void free(void *ptr);

    std::size_t totalSize() const { return totalSize_; }

private:
    void *region_{nullptr};
    std::size_t totalSize_{0};
};

} // namespace dcms
