#pragma once

#include "common/dcms_types.h"
#include "shm/ring_buffer.h"
#include "utils/dcms_thread.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <poll.h>
#include <sys/mman.h>
#include <unistd.h>

namespace dcms {

template <typename T>
class Subscriber {
public:
    using Callback = std::function<void(const T &)>;

    Subscriber() = default;

    // Constructed by NodeHandle::subscribe<T>()
    Subscriber(TopicId topic_id, void *ring_base, std::uint64_t shm_size,
               std::uint32_t ring_slots, std::uint32_t slot_size,
               int eventfd_fd, Callback cb)
        : topic_id_(topic_id),
          ring_base_(ring_base),
          shm_size_(shm_size),
          ring_(ring_base, ring_slots, slot_size),
          eventfd_fd_(eventfd_fd),
          cb_(std::move(cb)),
          running_(true) {
        recv_thread_ = std::make_unique<dcms::utils::SingleThread>(
            "recv_" + std::to_string(topic_id),
            [this] { receiveLoop(); });
    }

    ~Subscriber() {
        running_ = false;
        recv_thread_.reset();   // SingleThread dtor joins
        if (eventfd_fd_ >= 0) {
            ::close(eventfd_fd_);
            eventfd_fd_ = -1;
        }
        if (ring_base_) {
            ::munmap(ring_base_, shm_size_);
            ring_base_ = nullptr;
        }
    }

    Subscriber(Subscriber &&o) noexcept
        : topic_id_(o.topic_id_),
          ring_base_(o.ring_base_),
          shm_size_(o.shm_size_),
          ring_(std::move(o.ring_)),
          eventfd_fd_(o.eventfd_fd_),
          cb_(std::move(o.cb_)),
          read_idx_(o.read_idx_),
          running_(o.running_.load()),
          recv_thread_(std::move(o.recv_thread_)) {

        o.ring_base_   = nullptr;
        o.eventfd_fd_  = -1;
        o.running_     = false;
    }

    Subscriber &operator=(Subscriber &&o) noexcept {
        if (this == &o) return *this;
        this->~Subscriber();
        new (this) Subscriber(std::move(o));
        return *this;
    }

    Subscriber(const Subscriber &)            = delete;
    Subscriber &operator=(const Subscriber &) = delete;

    bool    valid()   const { return ring_base_ != nullptr; }
    TopicId topicId() const { return topic_id_; }

private:
    void receiveLoop() {
        while (running_.load(std::memory_order_relaxed)) {
            pollfd pfd{eventfd_fd_, POLLIN, 0};
            // 100ms 超时：running_=false 后最多再等 100ms 才退出
            int rc = ::poll(&pfd, 1, 100 /*ms*/);
            if (rc <= 0) continue;

            if (pfd.revents & (POLLHUP | POLLERR)) {
                // Publisher 进程退出，eventfd 被关闭，POLLHUP 触发
                running_ = false;
                break;
            }

            if (!(pfd.revents & POLLIN)) continue;

            // read(eventfd) 一次原子地读出并清零计数器，得到自上次唤醒以来积压的消息数
            std::uint64_t count = 0;
            if (::read(eventfd_fd_, &count, sizeof(count)) != sizeof(count))
                continue;

            // 直接对比 ring 的 write_idx 和本地 read_idx_：
            // 防止 Publisher 在 eventfd read 之前已经连写了多条，count 比实际少的情况
            const RingHeader *hdr = ring_.header();
            const std::uint32_t writer_idx =
                hdr->write_idx.load(std::memory_order_acquire);

            std::uint32_t pending =
                static_cast<std::uint32_t>(writer_idx - read_idx_);
            if (pending == 0) pending = static_cast<std::uint32_t>(count); // 已追上，按 eventfd 计数走

            for (std::uint32_t i = 0; i < pending; ++i) {
                alignas(alignof(T)) std::uint8_t buf[sizeof(T)];
                std::uint32_t out_len = 0, out_seq = 0;

                // seqlock 读取失败（正在写入/从未写入）→ 跳出，下次 eventfd 唤醒再读
                if (!ring_.read(read_idx_, buf, sizeof(T), out_len, out_seq))
                    break;

                if (cb_ && out_len >= sizeof(T))
                    cb_(*reinterpret_cast<const T *>(buf));

                ++read_idx_; // 每个 Subscriber 独立维护读游标，不影响其他 Subscriber
            }
        }
    }

    TopicId       topic_id_{0};
    void         *ring_base_{nullptr};
    std::uint64_t shm_size_{0};
    RingBuffer    ring_;
    int           eventfd_fd_{-1};
    Callback      cb_;
    std::uint32_t read_idx_{0};
    std::atomic<bool>                          running_{false};
    std::unique_ptr<dcms::utils::SingleThread> recv_thread_;
};

} // namespace dcms
