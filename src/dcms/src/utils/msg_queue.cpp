#include "utils/msg_queue.h"

namespace dcms {

// capacity+1 是为了让 head==tail 表示"空"，(head+1)==tail 表示"满"，两种状态不混淆
MsgQueue::MsgQueue(std::size_t capacity)
    : capacity_(capacity + 1), buf_(capacity + 1, nullptr) {}

bool MsgQueue::push(void *msg) {
    const std::size_t cur  = head_.load(std::memory_order_relaxed); // 只有生产者改 head，relaxed 读自己的值
    const std::size_t next = (cur + 1) % capacity_;
    if (next == tail_.load(std::memory_order_acquire)) // acquire：确保看到消费者最新的 tail
        return false; // 队列满
    buf_[cur] = msg;
    head_.store(next, std::memory_order_release); // release：让消费者 acquire 时能看到 buf_[cur] 已写入
    return true;
}

bool MsgQueue::pop(void *&msg) {
    const std::size_t cur = tail_.load(std::memory_order_relaxed); // 只有消费者改 tail
    if (cur == head_.load(std::memory_order_acquire)) // acquire：确保看到生产者最新的 head 和 buf_
        return false; // 队列空
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
