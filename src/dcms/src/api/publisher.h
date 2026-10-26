#pragma once

#include "dcms_export.h"
#include "common/dcms_types.h"
#include "common/dcms_msg.h"
#include "common/e2e.h"
#include "shm/ring_buffer.h"
#include "shm/shm_defs.h"
#include "shm/shm_pool.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

namespace dcms {

class NodeHandle;

// =============================================================================
// Publisher — 零拷贝发布者（对齐公司生产版架构）
//
// 数据流（publish<T> 路径）：
//   1. ShmPool::alloc_handle(sizeof(T))  — 从共享内存池取一块内存，refcount=1
//   2. 填写用户数据 T（1 次 memcpy：用户栈 → ShmPool）
//   3. 填写 MsgMeta + E2E 保护
//   4. ring_.write([MsgMeta + ShmHandle])  — 只写 64B 引用进 ring（不含 T！）
//   5. notifySubscribers()  — eventfd +1 唤醒 Subscriber 线程
//
// 数据流（loan<T> / commit 零拷贝路径）：
//   1. ShmPool::alloc_handle(sizeof(T))  — 取内存，refcount=1
//   2. loan.payload 直接指向 ShmPool，用户原地写（零 memcpy！）
//   3. commit()  — E2E 保护，将 MsgMeta + ShmHandle 写入 ring，通知 Subscriber
//
// Subscriber 端收到 ShmHandle 后通过 ShmPool 直接访问 T，不再经 ring 拷贝。
// =============================================================================
class DCMS_API Publisher {
public:
    Publisher() = default;

    Publisher(TopicId topic_id, void *ring_base, std::uint64_t shm_size,
              std::uint32_t ring_slots, std::uint32_t slot_size,
              NodeHandle *owner,
              TopicPriority priority = TopicPriority::kNormal);

    ~Publisher();
    Publisher(Publisher &&) noexcept;
    Publisher &operator=(Publisher &&) noexcept;
    Publisher(const Publisher &)            = delete;
    Publisher &operator=(const Publisher &) = delete;

    bool          valid()    const { return ring_base_ != nullptr; }
    TopicId       topicId()  const { return topic_id_; }
    TopicPriority priority() const { return priority_; }

    // ── 拷贝式发布（1 次 memcpy：用户栈 → ShmPool）────────────────────────────
    //
    // T 必须是 trivially copyable。
    // 内部为 T 分配 ShmPool 内存 → 拷入数据 → 写 ring slot（MsgMeta + ShmHandle）。
    template <typename T>
    int publish(const T &msg,
                std::uint32_t module_id  = 0,
                std::uint32_t data_type  = 0,
                std::uint64_t ts_capture = 0) {
        static_assert(__is_trivially_copyable(T),
                      "Publisher<T>: T must be trivially copyable");

        if (!ring_base_) return kDcmsErrNotInit;
        if (!ShmPool::instance().ready()) return kDcmsErrNotInit;

        // 1. 从 ShmPool 分配 sizeof(T) 用户内存
        ShmHandle handle = ShmPool::instance().alloc_handle(sizeof(T));
        if (!handle.valid()) return kDcmsErrNoMemory;

        // 2. 拷贝用户数据到 ShmPool（唯一一次 memcpy）
        void *shm_payload = ShmPool::instance().handle_payload(handle);
        std::memcpy(shm_payload, &msg, sizeof(T));

        // 3. 构建 ring slot payload：[MsgMeta 48B][ShmHandle 16B]
        std::uint8_t buf[kRingSlotPayloadSize];
        auto *meta    = reinterpret_cast<MsgMeta *>(buf);
        auto *shandle = reinterpret_cast<ShmHandle *>(buf + sizeof(MsgMeta));

        meta->module_id         = module_id;
        meta->instance_id       = topic_id_;
        meta->data_type         = data_type;
        meta->seq               = pub_seq_.fetch_add(1u, std::memory_order_relaxed);
        meta->timestamp_capture = ts_capture ? ts_capture : nowUs();
        meta->timestamp_publish = nowUs();
        meta->payload_len       = static_cast<std::uint32_t>(sizeof(T));
        meta->payload_crc32     = 0;
        meta->e2e_counter       = 0;
        meta->e2e_crc           = 0;

        e2e_protect(*meta, shm_payload, sizeof(T), e2e_counter_);
        *shandle = handle;

        return publish_raw(buf, kRingSlotPayloadSize);
    }

    // 原始发布接口：payload = 已组装好的 [MsgMeta + ShmHandle]（64B）
    int publish_raw(const void *data, std::uint32_t len);

    // ── 零拷贝发布（ShmPool 直写，无任何 memcpy）──────────────────────────────
    //
    // 使用方式：
    //   auto loan = pub.loan<LidarFrame>();            // 1. 从 ShmPool 取直写指针
    //   if (!loan.payload) return;
    //   loan.payload->points = ...;                    // 2. 直接写 ShmPool（零拷贝！）
    //   pub.commit<LidarFrame>(loan);                  // 3. 提交：ring + notify
    //
    // 中途出错调 cancelLoan() 回滚 ShmPool 分配 + ring seqlock。
    template <typename T>
    struct Loan {
        MsgMeta  *meta{nullptr};    ///< 指向 ring slot 内 MsgMeta（填写元数据）
        T        *payload{nullptr}; ///< 指向 ShmPool 中用户数据（直接写这里！）
        ShmHandle handle{};         ///< 会被写入 ring slot 的 ShmHandle
    };

    template <typename T>
    Loan<T> loan() {
        static_assert(__is_trivially_copyable(T),
                      "Publisher<T>: T must be trivially copyable");
        Loan<T> result{};
        if (!ring_base_ || !ShmPool::instance().ready()) return result;

        // 1. 从 ShmPool 分配 sizeof(T)
        ShmHandle handle = ShmPool::instance().alloc_handle(sizeof(T));
        if (!handle.valid()) return result;

        // 2. 开始 ring 写（seq → 奇数）
        auto wh = ring_.beginWrite();
        if (!wh.valid) {
            ShmPool::instance().free_handle(handle);
            return result;
        }

        pending_write_  = wh;
        pending_handle_ = handle;

        result.meta    = wh.meta;
        result.payload = reinterpret_cast<T *>(
            ShmPool::instance().handle_payload(handle));
        result.handle  = handle;
        return result;
    }

    // 提交 loan：E2E 保护，将 ShmHandle 写入 ring slot，推进 write_idx，通知 Subscriber。
    template <typename T>
    int commit(Loan<T> &l,
               std::uint32_t module_id  = 0,
               std::uint32_t data_type  = 0,
               std::uint64_t ts_capture = 0) {
        if (!l.payload || !pending_write_.valid)
            return kDcmsErrNotInit;

        MsgMeta *meta = l.meta;
        meta->module_id         = module_id;
        meta->instance_id       = topic_id_;
        meta->data_type         = data_type;
        meta->seq               = pub_seq_.fetch_add(1u, std::memory_order_relaxed);
        meta->timestamp_capture = ts_capture ? ts_capture : nowUs();
        meta->timestamp_publish = nowUs();
        meta->payload_len       = static_cast<std::uint32_t>(sizeof(T));
        meta->payload_crc32     = 0;
        meta->e2e_counter       = 0;
        meta->e2e_crc           = 0;

        // E2E 直接对 ShmPool 内存计算（零拷贝，无副本）
        e2e_protect(*meta, l.payload, sizeof(T), e2e_counter_);

        // 把 ShmHandle 写入 ring slot 的 shm_handle 区域
        *pending_write_.shm_handle = pending_handle_;

        ring_.endWrite(pending_write_, sizeof(ShmHandle));
        notifySubscribers();
        return kDcmsOk;
    }

    // 回滚未提交的 loan：释放 ShmPool 内存 + ring seqlock 回滚
    void cancelLoan();

    // 由 NodeHandle::notifyLoop() 调用，新订阅者加入时投递 eventfd
    int addSubscriberEventfd(int fd);

    // ── 通知策略（高频 topic 降 syscall 用）──────────────────────────────────
    //
    // 默认 max_batch=1：每帧写完立刻通知，语义与优化前一致。
    // 设为 >1 时，累积若干帧或超过 max_delay_us 才 notify 一次；订阅者被唤醒后
    // 通过 write_idx - read_idx_ 一次取走全部积压帧。这是用少量延迟换吞吐，
    // 只适合持续高码率的 topic（偶发发布的 topic 用它会让最后一帧延迟送达）。
    struct NotifyPolicy {
        std::uint32_t max_batch{1};     ///< 累积到多少帧通知一次，1=每帧通知
        std::uint32_t max_delay_us{0};  ///< 上次通知超过该时长也通知，0=不按时长
    };
    void setNotifyPolicy(const NotifyPolicy &p) { notify_policy_ = p; }
    NotifyPolicy notifyPolicy() const { return notify_policy_; }

private:
    void notifySubscribers();

    TopicId       topic_id_{0};
    void         *ring_base_{nullptr};
    std::uint64_t shm_size_{0};
    RingBuffer    ring_;
    NodeHandle   *owner_{nullptr};

    std::atomic<std::uint32_t> pub_seq_{0};
    std::uint32_t              e2e_counter_{0};
    TopicPriority              priority_{TopicPriority::kNormal};

    // loan 状态（单一未提交 loan）
    RingBuffer::WriteHandle pending_write_{};
    ShmHandle               pending_handle_{ShmHandle::invalid()};

    // 订阅者 fd 列表：读多写少（只在订阅关系变化时改）。
    // 发布侧每次 publish 都要遍历它，所以用 shared_ptr 快照 + 原子读取，
    // 把 publish 热路径上的互斥锁彻底去掉；写仍由 subs_mutex_ 串行化。
    mutable std::mutex                       subs_mutex_;
    std::shared_ptr<const std::vector<int>>  subscriber_eventfds_;

    NotifyPolicy     notify_policy_{};
    std::uint32_t    pending_frames_{0};   ///< 批模式下尚未通知的帧数
    std::uint64_t    last_notify_us_{0};   ///< 上次通知时刻（max_delay_us 用）
};

} // namespace dcms
