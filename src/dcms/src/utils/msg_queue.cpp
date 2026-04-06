#include "utils/msg_queue.h"

namespace dcms {

MsgQueue::MsgQueue(std::size_t capacity)
    : capacity_(capacity + 1), buf_(capacity + 1, nullptr) {}

bool MsgQueue::push(void *msg) {
    const std::size_t cur = head_.load(std::memory_order_relaxed);
    const std::size_t next = (cur + 1) % capacity_;
    if (next == tail_.load(std::memory_order_acquire))
        return false;
    buf_[cur] = msg;
    head_.store(next, std::memory_order_release);
    return true;
}

bool MsgQueue::pop(void *&msg) {
    const std::size_t cur = tail_.load(std::memory_order_relaxed);
    if (cur == head_.load(std::memory_order_acquire))
        return false;
    msg = buf_[cur];
    tail_.store((cur + 1) % capacity_, std::memory_order_release);
    return true;
}

bool MsgQueue::full() const {
    return ((head_.load(std::memory_order_relaxed) + 1) % capacity_) ==
           tail_.load(std::memory_order_relaxed);
}

bool MsgQueue::empty() const {
    return head_.load(std::memory_order_relaxed) == tail_.load(std::memory_order_relaxed);
}

} // namespace dcms
