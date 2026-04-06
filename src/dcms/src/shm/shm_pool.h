#pragma once

#include "dcms_export.h"

#include <cstddef>
#include <string>

namespace dcms {

class BuddyAllocator;
class SlabAllocator;

class DCMS_API ShmPool {
public:
    static ShmPool &instance();

    ShmPool(const ShmPool &) = delete;
    ShmPool &operator=(const ShmPool &) = delete;

    bool create(const std::string &name, std::size_t size);
    bool attach(const std::string &name);
    void destroy();

    void *alloc(std::size_t size);
    void free(void *ptr);

    bool ready() const { return region_ != nullptr; }
    std::size_t poolSize() const { return poolSize_; }

private:
    ShmPool();
    ~ShmPool();

    void *region_{nullptr};
    std::size_t poolSize_{0};
    int shmFd_{-1};
    std::string shmName_;

    BuddyAllocator *buddy_{nullptr};
    SlabAllocator *slab_{nullptr};
};

} // namespace dcms
