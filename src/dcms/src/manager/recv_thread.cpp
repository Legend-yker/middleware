// =============================================================================
// recv_thread.cpp —— 后台消息接收线程
//
// 这是消息"收件箱"的核心逻辑：
//   - 一个独立线程持续轮询 IPC 通道
//   - 收到消息后做 CRC 校验，然后按 topic_id 分发给对应的回调函数
//   - 维护一张"订阅表"：topic_id → [ 回调函数列表 ]
// =============================================================================

#include "manager/recv_thread.h"
#include "common/crc32.h"

#include <algorithm>
#include <cstring>

namespace dcms {

RecvThread::RecvThread(std::shared_ptr<IpcBackend> backend)
    : backend_(std::move(backend)) {}

RecvThread::~RecvThread() { stop(); }

// -----------------------------------------------------------------------------
// subscribe: 向订阅表添加一个回调
//
// callback_key + user_key 共同唯一标识一个订阅（用于 unsubscribe 时定位）
// 同一个 topic 可以有多个订阅者，每个都会收到消息
// -----------------------------------------------------------------------------
void RecvThread::subscribe(TopicId topic, std::uintptr_t callback_key,
                           void *user_key, MsgCallback cb) {
    std::lock_guard<std::mutex> lk(sub_mutex_);
    // subs_ 是一个 map：topic_id → vector<SubscriptionEntry>
    subs_[topic].push_back(SubscriptionEntry{
        callback_key,
        user_key,
        std::move(cb),
    });
}

// -----------------------------------------------------------------------------
// unsubscribe: 从订阅表移除指定回调
// -----------------------------------------------------------------------------
void RecvThread::unsubscribe(TopicId topic, std::uintptr_t callback_key, void *user_key) {
    std::lock_guard<std::mutex> lk(sub_mutex_);
    auto it = subs_.find(topic);
    if (it == subs_.end())
        return;

    auto &entries = it->second;
    // 用 erase-remove 惯用法：找到匹配的 entry 删除
    entries.erase(
        std::remove_if(entries.begin(), entries.end(),
                       [&](const SubscriptionEntry &e) {
                           return e.callback_key == callback_key &&
                                  e.user_key == user_key;
                       }),
        entries.end());

    // 该 topic 下没有订阅者了，从 map 中移除
    if (entries.empty())
        subs_.erase(it);
}

bool RecvThread::start() {
    if (running_.exchange(true))
        return false; // 已经在跑了
    worker_ = std::thread([this] { loop(); });
    return true;
}

void RecvThread::stop() {
    running_ = false;
    // 等后台线程自然退出（recvNotify 超时 50ms 后会检查 running_ 标志）
    if (worker_.joinable())
        worker_.join();
}

// -----------------------------------------------------------------------------
// loop: 后台线程的主循环
//
// 流程图：
//
//   ┌─────────────────────────────────────────────┐
//   │  while(running)                             │
//   │    ↓                                        │
//   │  recvNotify(timeout=50ms)                   │
//   │    ↓ 超时? → continue（检查 running 标志）   │
//   │    ↓ 收到消息 ptr                            │
//   │  校验 ptr 不为空 且 len >= header 大小        │
//   │    ↓                                        │
//   │  CRC32 校验                                  │
//   │    ↓ 失败? → 丢弃，释放内存，continue        │
//   │    ↓ 通过                                   │
//   │  dispatch(hdr) → 分发给订阅者回调            │
//   │    ↓                                        │
//   │  releaseRecvBuffer(ptr) → 释放内存           │
//   └─────────────────────────────────────────────┘
// -----------------------------------------------------------------------------
void RecvThread::loop() {
    while (running_.load(std::memory_order_relaxed)) {
        void *ptr = nullptr;
        std::size_t len = 0;

        // 阻塞等待 IPC 通知，最多等 50ms（保证 stop() 能及时响应）
        // ptr 收到后指向一块内存：[DcmsMsgHeader][用户数据]
        if (!backend_->recvNotify(ptr, len, 50))
            continue;

        // 基本完整性检查
        if (!ptr || len < sizeof(DcmsMsgHeader))
            continue;

        auto *hdr = static_cast<DcmsMsgHeader *>(ptr);

        // CRC 校验：
        //   原理：发布时 crc32 字段清零后计算，结果写入 crc32
        //         接收时再次清零计算，比对是否一致
        //   作用：检测内存损坏、传输错误、恶意篡改
        const std::uint32_t stored_crc = hdr->crc32;
        hdr->crc32 = 0;
        const std::uint32_t actual_crc = crc32(hdr, sizeof(DcmsMsgHeader) + hdr->data_len);
        hdr->crc32 = stored_crc; // 还原（dispatch 可能还需要用到 header）
        if (actual_crc != stored_crc) {
            backend_->releaseRecvBuffer(ptr);
            continue; // 数据损坏，丢弃
        }

        // 分发消息给所有订阅了这个 topic 的回调
        dispatch(hdr);

        // 释放内存（接收方用完了，归还给内存池）
        backend_->releaseRecvBuffer(ptr);
    }
}

// -----------------------------------------------------------------------------
// dispatch: 把一条消息分发给所有订阅了该 topic 的回调
//
// 关键设计：为什么要"先复制列表，再调回调"？
//
//   如果在持锁状态下直接调用回调，用户的回调函数里又调了 subscribe()，
//   subscribe() 会尝试加锁 sub_mutex_ → 死锁！
//
//   解决方案：
//     ① 持锁 → 把订阅列表复制一份到局部变量
//     ② 释放锁
//     ③ 用局部副本调用回调（此时不持任何锁）
// -----------------------------------------------------------------------------
void RecvThread::dispatch(const DcmsMsgHeader *hdr) {
    std::vector<SubscriptionEntry> local_subs;
    {
        std::lock_guard<std::mutex> lk(sub_mutex_);
        auto it = subs_.find(hdr->topic_id);
        if (it == subs_.end())
            return; // 没有订阅者，直接返回
        local_subs = it->second; // 复制一份（浅拷贝，回调 lambda 含 shared_ptr 所以安全）
    }
    // 锁已释放，现在安全地调用每个回调
    // data = header 后面紧跟的用户数据首地址
    const void *data = msgDataPtr(hdr);
    for (auto &entry : local_subs)
        entry.cb(hdr, data, hdr->data_len);
}

} // namespace dcms
