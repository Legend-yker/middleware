#include "api/publisher.h"
#include "api/node_handle.h"

#include <cerrno>
#include <cstring>
#include <unistd.h>
#include <sys/mman.h>

namespace dcms {

Publisher::Publisher(TopicId topic_id, void *ring_base, std::uint64_t shm_size,
                     std::uint32_t ring_slots, std::uint32_t slot_size,
                     NodeHandle *owner, TopicPriority priority)
    : topic_id_(topic_id),
      ring_base_(ring_base),
      shm_size_(shm_size),
      ring_(ring_base, ring_slots, slot_size),
      owner_(owner),
      priority_(priority) {
    if (owner_)
        owner_->registerPublisher(topic_id_, this);
}

Publisher::~Publisher() {
    if (owner_)
        owner_->unregisterPublisher(topic_id_);

    if (auto snap = std::atomic_load(&subscriber_eventfds_); snap) {
        std::lock_guard<std::mutex> lk(subs_mutex_);
        for (int fd : *snap)
            ::close(fd);
        std::atomic_store(&subscriber_eventfds_,
                          std::shared_ptr<const std::vector<int>>{});
    }

    // 归还本 ring 仍持有的全部消息引用（槽内残留帧 + retire 队列），
    // 否则每个被销毁的 Publisher 都会永久漏掉约 2×ring_slots 份数据。
    ring_.releaseAll();

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
      priority_(o.priority_),
      subscriber_eventfds_(std::move(o.subscriber_eventfds_)) {
    o.ring_base_ = nullptr;
    o.owner_     = nullptr;
    if (owner_) {
        owner_->unregisterPublisher(topic_id_);
        owner_->registerPublisher(topic_id_, this);
    }
}

Publisher &Publisher::operator=(Publisher &&o) noexcept {
    if (this == &o) return *this;
    this->~Publisher();
    new (this) Publisher(std::move(o));
    return *this;
}

void Publisher::notifySubscribers() {
    // 热路径：只读快照，不加锁。订阅者列表是读多写少，写侧通过
    // atomic_store 整体替换，读者永远拿到一个不会被修改的 const vector。
    auto snapshot = std::atomic_load_explicit(&subscriber_eventfds_,
                                              std::memory_order_acquire);
    if (!snapshot || snapshot->empty()) return;

    // 批量通知：高频 topic 累积若干帧再唤醒一次，订阅者一次 read 取走全部积压。
    // 默认 max_batch=1（每帧通知），语义与优化前完全一致。
    if (notify_policy_.max_batch > 1) {
        ++pending_frames_;
        if (pending_frames_ < notify_policy_.max_batch) {
            if (notify_policy_.max_delay_us == 0) return;
            const std::uint64_t now = nowUs();
            if (last_notify_us_ != 0 &&
                now - last_notify_us_ < notify_policy_.max_delay_us)
                return;
        }
    }
    pending_frames_  = 0;
    last_notify_us_  = nowUs();

    const std::uint64_t val = 1u;
    int dead_fd = -1;
    for (int fd : *snapshot) {
        if (::write(fd, &val, sizeof(val)) < 0 &&
            (errno == EBADF || errno == EPIPE || errno == EINVAL))
            dead_fd = fd;   // 对端已消失，交处理
    }
    if (dead_fd >= 0) {
        // 罕见路径才加锁：剔除失效 fd（每次重建成本可接受）
        std::lock_guard<std::mutex> lk(subs_mutex_);
        auto cur = std::atomic_load(&subscriber_eventfds_);
        if (cur) {
            auto next = std::make_shared<std::vector<int>>();
            for (int fd : *cur)
                if (fd != dead_fd) next->push_back(fd);
            std::atomic_store(&subscriber_eventfds_,
                              std::shared_ptr<const std::vector<int>>(
                                  std::move(next)));
        }
        ::close(dead_fd);
    }
}

int Publisher::publish_raw(const void *data, std::uint32_t len) {
    if (!ring_base_) return kDcmsErrNotInit;
    if (!ring_.write(data, len)) return kDcmsErrInvalidArg;
    notifySubscribers();
    return kDcmsOk;
}

void Publisher::cancelLoan() {
    if (pending_write_.valid)
        ring_.cancelWrite(pending_write_);
    if (pending_handle_.valid())
        ShmPool::instance().free_handle(pending_handle_);
    pending_handle_ = ShmHandle::invalid();
}

int Publisher::addSubscriberEventfd(int fd) {
    // 写路径：整体复制一份新列表后原子替换（copy-on-write），
    // 读者持有的旧快照不受影响，析构时由 shared_ptr 自动回收。
    std::lock_guard<std::mutex> lk(subs_mutex_);
    auto cur  = std::atomic_load(&subscriber_eventfds_);
    auto next = cur ? std::vector<int>(*cur) : std::vector<int>{};
    next.push_back(fd);
    std::atomic_store(&subscriber_eventfds_,
                      std::shared_ptr<const std::vector<int>>(
                          std::make_shared<std::vector<int>>(std::move(next))));
    return 0;
}

} // namespace dcms
