#pragma once

#include "dcms_export.h"

#include <cstddef>

namespace dcms {

class DCMS_API SlabAllocator {
public:
    SlabAllocator();
    ~SlabAllocator();

    SlabAllocator(const SlabAllocator &) = delete;
    SlabAllocator &operator=(const SlabAllocator &) = delete;

    bool init(void *region, std::size_t totalSize, std::size_t objectSize);

    void *alloc();
    void free(void *ptr);

    std::size_t objectSize() const { return objectSize_; }

private:
    void *region_{nullptr};
    std::size_t totalSize_{0};
    std::size_t objectSize_{0};
};

} // namespace dcms
