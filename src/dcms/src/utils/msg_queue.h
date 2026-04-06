#pragma once

#include "dcms_export.h"

#include <atomic>
#include <cstddef>
#include <vector>

namespace dcms {

class DCMS_API MsgQueue {
public:
    explicit MsgQueue(std::size_t capacity = 256);
    ~MsgQueue() = default;

    MsgQueue(const MsgQueue &) = delete;
    MsgQueue &operator=(const MsgQueue &) = delete;

    bool push(void *msg);
    bool pop(void *&msg);
    bool full() const;
    bool empty() const;
    std::size_t capacity() const { return capacity_; }

private:
    std::size_t capacity_;
    std::vector<void *> buf_;
    std::atomic<std::size_t> head_{0};
    std::atomic<std::size_t> tail_{0};
};

} // namespace dcms
