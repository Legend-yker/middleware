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

    // Constructed by NodeHandle::advertise<T>()
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

    // Write msg directly into the ring buffer, then signal all subscribers.
    // T must be trivially copyable and sizeof(T) == slot_size_ (enforced at runtime).
    template <typename T>
    int publish(const T &msg) {
        static_assert(__is_trivially_copyable(T),
                      "Publisher<T>: T must be trivially copyable");
        return publish(static_cast<const void *>(&msg),
                       static_cast<std::uint32_t>(sizeof(T)));
    }

    // Raw publish
    int publish(const void *data, std::uint32_t len);

    // Called by NodeHandle::notifyLoop() when a new subscriber joins.
    // Takes ownership of fd.
    void addSubscriberEventfd(int fd);

private:
    TopicId       topic_id_{0};
    void         *ring_base_{nullptr};
    std::uint64_t shm_size_{0};
    RingBuffer    ring_;
    NodeHandle   *owner_{nullptr};

    std::mutex       eventfds_mutex_;
    std::vector<int> subscriber_eventfds_;
};

} // namespace dcms
