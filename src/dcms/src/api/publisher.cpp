#include "api/publisher.h"
#include "api/node_handle.h"

#include <cerrno>
#include <cstring>
#include <unistd.h>
#include <sys/mman.h>

namespace dcms {

Publisher::Publisher(TopicId topic_id, void *ring_base, std::uint64_t shm_size,
                     std::uint32_t ring_slots, std::uint32_t slot_size,
                     NodeHandle *owner)
    : topic_id_(topic_id),
      ring_base_(ring_base),
      shm_size_(shm_size),
      ring_(ring_base, ring_slots, slot_size),
      owner_(owner) {
    if (owner_)
        owner_->registerPublisher(topic_id_, this);
}

Publisher::~Publisher() {
    if (owner_)
        owner_->unregisterPublisher(topic_id_);

    {
        std::lock_guard<std::mutex> lk(eventfds_mutex_);
        for (int fd : subscriber_eventfds_)
            ::close(fd);
        subscriber_eventfds_.clear();
    }

    if (ring_base_) {
        ::munmap(ring_base_, shm_size_);
        ring_base_ = nullptr;
    }
}

Publisher::Publisher(Publisher &&o) noexcept
    : topic_id_(o.topic_id_),
      ring_base_(o.ring_base_),
      shm_size_(o.shm_size_),
      ring_(std::move(o.ring_)),
      owner_(o.owner_),
      subscriber_eventfds_(std::move(o.subscriber_eventfds_)) {
    o.ring_base_ = nullptr;
    o.owner_     = nullptr;
    if (owner_) {
        owner_->unregisterPublisher(topic_id_);   // remove old ptr
        owner_->registerPublisher(topic_id_, this); // register new ptr
    }
}

Publisher &Publisher::operator=(Publisher &&o) noexcept {
    if (this == &o) return *this;
    this->~Publisher();
    new (this) Publisher(std::move(o));
    return *this;
}

int Publisher::publish(const void *data, std::uint32_t len) {
    if (!ring_base_)
        return kDcmsErrNotInit;

    if (!ring_.write(data, len))
        return kDcmsErrInvalidArg;  // len > slot_size

    // 写 eventfd：每次 write(fd, &1, 8) 使计数器 +1，Subscriber 的 poll 被唤醒
    const std::uint64_t val = 1u;
    std::lock_guard<std::mutex> lk(eventfds_mutex_);
    for (auto it = subscriber_eventfds_.begin();
         it != subscriber_eventfds_.end(); ) {
        if (::write(*it, &val, sizeof(val)) < 0 &&
            (errno == EBADF || errno == EPIPE || errno == EINVAL)) {
            // Subscriber 已退出（fd 已关闭）→ 自动清理，防止 fd 泄漏
            ::close(*it);
            it = subscriber_eventfds_.erase(it);
        } else {
            ++it;
        }
    }
    return kDcmsOk;
}

int Publisher::addSubscriberEventfd(int fd) {
    std::lock_guard<std::mutex> lk(eventfds_mutex_);
    subscriber_eventfds_.push_back(fd);
    return 0;
}

} // namespace dcms
