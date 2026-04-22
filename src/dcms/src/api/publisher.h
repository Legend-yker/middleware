#pragma once

#include "dcms_export.h"
#include "common/dcms_types.h"
#include "shm/ring_buffer.h"

#include <cstdint>
#include <mutex>
#include <vector>

namespace dcms {

class NodeHandle;

class DCMS_API Publisher {
public:
    Publisher() = default;

    // 由 NodeHandle::advertise<T>() 构造，外部不直接 new
    Publisher(TopicId topic_id, void *ring_base, std::uint64_t shm_size,
              std::uint32_t ring_slots, std::uint32_t slot_size,
              NodeHandle *owner);

    ~Publisher();

    Publisher(Publisher &&) noexcept;
    Publisher &operator=(Publisher &&) noexcept;
    Publisher(const Publisher &)            = delete;
    Publisher &operator=(const Publisher &) = delete;

    bool    valid()   const { return ring_base_ != nullptr; }
    TopicId topicId() const { return topic_id_; }

    // 将 msg 写入环形缓冲区并通过 eventfd 通知所有订阅者。
    // T 必须是 trivially copyable（可安全 memcpy，无虚表/资源句柄）。
    template <typename T>
    int publish(const T &msg) {
        static_assert(__is_trivially_copyable(T),
                      "Publisher<T>: T must be trivially copyable");
        return publish(static_cast<const void *>(&msg),
                       static_cast<std::uint32_t>(sizeof(T)));
    }

    // 不指定类型的原始发布接口（SensorBase 等运行时确定大小时使用）
    int publish(const void *data, std::uint32_t len);

    // 由 NodeHandle::notifyLoop() 调用，新订阅者加入时投递 eventfd
    int addSubscriberEventfd(int fd);

private:
    TopicId       topic_id_{0};
    void         *ring_base_{nullptr};  // mmap 共享内存起始地址
    std::uint64_t shm_size_{0};         // mmap 大小，析构时 munmap 用
    RingBuffer    ring_;                // 非持有型访问器，指向 ring_base_
    NodeHandle   *owner_{nullptr};      // 反向引用，不持有所有权

    std::mutex       eventfds_mutex_;
    std::vector<int> subscriber_eventfds_; // 每个订阅者一个 eventfd，publish 时逐一通知
};

} // namespace dcms
