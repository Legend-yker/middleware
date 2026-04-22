#pragma once

#include "dcms_export.h"

#include <atomic>
#include <cstddef>
#include <vector>

namespace dcms {

// 单生产者单消费者无锁环形队列，存放 void* 指针（指向消息对象）。
// head_ 由生产者独占写，tail_ 由消费者独占写，两者用 atomic 保证可见性。
// capacity_ = 用户传入值 + 1，多出的一个槽用于区分"满"和"空"。
class DCMS_API MsgQueue {
public:
    explicit MsgQueue(std::size_t capacity = 256);
    ~MsgQueue() = default;

    MsgQueue(const MsgQueue &) = delete;
    MsgQueue &operator=(const MsgQueue &) = delete;

    bool push(void *msg);       // 队列满时返回 false，不阻塞
    bool pop(void *&msg);       // 队列空时返回 false，不阻塞
    bool full() const;
    bool empty() const;
    std::size_t capacity() const { return capacity_; }

private:
    std::size_t              capacity_;  // 实际数组长度 = 用户容量 + 1
    std::vector<void *>      buf_;
    std::atomic<std::size_t> head_{0};  // 生产者写入位置（下一个空槽）
    std::atomic<std::size_t> tail_{0};  // 消费者读取位置（下一个待读槽）
};

} // namespace dcms
