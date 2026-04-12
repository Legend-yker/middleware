// =============================================================================
// dcms_api.cpp —— DCMS 核心 C API 实现
//
// 这是整个发布订阅系统的"大脑"，负责：
//   1. 初始化：创建共享内存池、IPC 通道、接收线程
//   2. 发布：把消息写入共享内存，通过 IPC 发送通知
//   3. 订阅/取消订阅：维护回调函数注册表
// =============================================================================

#include "api/dcms_api.h"

#include "common/crc32.h"
#include "common/dcms_msg.h"
#include "common/dcms_types.h"
#include "manager/recv_thread.h"
#include "shm/shm_pool.h"
#include "transport/common/ipc_backend.h"

#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace {

// -----------------------------------------------------------------------------
// 全局上下文：整个进程只有一个 DCMS 实例
// -----------------------------------------------------------------------------
struct DcmsContext {
    std::shared_ptr<dcms::IpcBackend>  ipc;          // IPC 传输层（Linux/QNX 自动选择）
    std::unique_ptr<dcms::RecvThread>  recv_thread;  // 后台接收线程
    std::atomic<std::uint32_t>         seq_counter{0}; // 全局消息序列号，每次发布自动+1
    std::unordered_map<dcms::TopicId, int> topic_ref_counts; // 每个 topic 的订阅者计数
    bool inited{false};
};

std::mutex   g_ctx_mutex; // 保护 g_ctx 的互斥锁（init/shutdown/subscribe 都要持锁）
DcmsContext  g_ctx;

} // namespace

extern "C" {

// -----------------------------------------------------------------------------
// dcms_init: 系统初始化，整个进程只需调用一次
//
// 初始化顺序：
//   ① 创建共享内存池（4MB）
//   ② 选择 IPC 后端（Linux→UnixSocket，QNX→Pulse）并初始化
//   ③ 启动后台接收线程
// -----------------------------------------------------------------------------
int dcms_init(const char *config_path) {
    (void)config_path;
    std::lock_guard<std::mutex> lk(g_ctx_mutex);
    if (g_ctx.inited)
        return dcms::kDcmsOk; // 重复调用直接返回，幂等

    // ① 创建共享内存池，4MB 供所有消息共用
    if (!dcms::ShmPool::instance().create("dcms_shm", 4 * 1024 * 1024))
        return dcms::kDcmsErrShmFail;

    // ② 自动选择 IPC 后端并初始化通道名 "dcms_main"
    g_ctx.ipc = std::shared_ptr<dcms::IpcBackend>(dcms::createIpcBackend());
    if (!g_ctx.ipc || !g_ctx.ipc->init("dcms_main"))
        return dcms::kDcmsErrNotImpl;

    // ③ 启动后台线程，持续监听 IPC 通知并分发消息
    g_ctx.recv_thread = std::make_unique<dcms::RecvThread>(g_ctx.ipc);
    g_ctx.recv_thread->start();

    g_ctx.inited = true;
    return dcms::kDcmsOk;
}

// -----------------------------------------------------------------------------
// dcms_shutdown: 关闭系统，释放所有资源
// -----------------------------------------------------------------------------
void dcms_shutdown(void) {
    std::lock_guard<std::mutex> lk(g_ctx_mutex);
    if (!g_ctx.inited)
        return;
    if (g_ctx.recv_thread)
        g_ctx.recv_thread->stop();      // 等后台线程退出（最多等 50ms）
    g_ctx.recv_thread.reset();
    if (g_ctx.ipc)
        g_ctx.ipc->shutdown();           // 关闭 socket/IPC 通道
    g_ctx.ipc.reset();
    dcms::ShmPool::instance().destroy(); // 释放共享内存
    g_ctx.inited = false;
}

// -----------------------------------------------------------------------------
// dcms_msg_publish: 发布一条消息
//
// 完整流程：
//   ① 从 ShmPool 申请一块内存（Header + 数据）
//   ② 填写消息头（topic、序列号、时间戳等）
//   ③ 把用户数据 memcpy 进去
//   ④ 计算整块内存的 CRC32（保证接收方能检测到数据损坏）
//   ⑤ 通过 IPC 后端发送"通知"（socketpair 模式只发指针，broker 模式发完整数据）
//   ⑥ 如果不是 SHM 传输，立即释放内存（broker 模式下数据已序列化发走了）
// -----------------------------------------------------------------------------
int dcms_msg_publish(dcms_topic_id topic, const void *data, std::uint32_t len) {
    if (!g_ctx.inited)
        return dcms::kDcmsErrNotInit;

    // ① 申请内存：布局为 [DcmsMsgHeader][用户数据]
    const std::size_t total_size = sizeof(dcms::DcmsMsgHeader) + len;
    void *buf = dcms::ShmPool::instance().alloc(total_size);
    if (!buf)
        return dcms::kDcmsErrShmFail;

    // ② 填写消息头
    auto *hdr = static_cast<dcms::DcmsMsgHeader *>(buf);
    std::memset(hdr, 0, sizeof(dcms::DcmsMsgHeader));
    hdr->topic_id    = topic;
    hdr->seq         = g_ctx.seq_counter.fetch_add(1, std::memory_order_relaxed); // 原子自增
    hdr->msg_type    = dcms::MsgType::kPublish;
    hdr->data_offset = static_cast<std::uint32_t>(sizeof(dcms::DcmsMsgHeader)); // 数据紧跟 header
    hdr->data_len    = len;

    // ③ 把用户数据复制到 header 后面
    //    msgDataPtr(hdr) = (char*)hdr + hdr->data_offset
    if (data && len > 0)
        std::memcpy(dcms::msgDataPtr(hdr), data, len);

    // ④ 记录三个时间戳（当前都相同，接收后 transfer 会被更新）
    const std::uint64_t ts = dcms::nowUs();
    hdr->timestamp_publish  = ts; // 发布时刻（业务层）
    hdr->timestamp_system   = ts; // 系统时刻
    hdr->timestamp_transfer = ts; // 传输时刻（接收方收到后可更新）

    // ⑤ CRC32：先清零 crc32 字段，对整块内存（含 header+data）计算，再写回
    //    这样接收方用同样方式验证，能检测任何比特翻转
    hdr->crc32 = 0;
    hdr->crc32 = dcms::crc32(hdr, total_size);

    // ⑥ 发送通知给 IPC 后端
    //    - socketpair 模式：只发 8 字节 {ptr, len}，接收方直接访问 buf 指针
    //    - broker 模式：把完整数据序列化后发给 broker 进程
    if (!g_ctx.ipc->sendNotify(buf, total_size)) {
        dcms::ShmPool::instance().free(buf); // 发送失败，立即释放
        return dcms::kDcmsErrBusy;
    }

    // ⑦ broker 模式下数据已拷贝发出，ShmPool 的 buf 不再需要，立即释放
    //    socketpair 模式下接收方还在用这个 buf，由接收方负责释放
    if (!g_ctx.ipc->usesSharedMemoryTransport())
        dcms::ShmPool::instance().free(buf);

    return dcms::kDcmsOk;
}

// -----------------------------------------------------------------------------
// dcms_msg_subscribe: 订阅某个 topic
//
// 参数：
//   topic    - 要监听的 topic id
//   cb       - 收到消息时调用的函数（在 RecvThread 线程中被调用！）
//   user     - 传给 cb 的自定义指针（通常是对象 this 指针）
//
// 内部流程：
//   ① 首次订阅某 topic → 告知 IPC 层开始接收这个 topic（broker 模式需要）
//   ② 在 RecvThread 的订阅表中登记 lambda 回调
//   ③ 维护引用计数（同一 topic 多次订阅不重复注册 IPC）
// -----------------------------------------------------------------------------
int dcms_msg_subscribe(dcms_topic_id topic, dcms_msg_callback cb, void *user) {
    if (!cb)
        return dcms::kDcmsErrInvalidArg;

    std::lock_guard<std::mutex> lk(g_ctx_mutex);
    if (!g_ctx.inited)
        return dcms::kDcmsErrNotInit;

    // 引用计数为 0 说明这是该 topic 的第一个订阅者，需要向 IPC 层注册
    int &ref_count = g_ctx.topic_ref_counts[topic];
    if (ref_count == 0 && !g_ctx.ipc->subscribeTopic(topic)) {
        g_ctx.topic_ref_counts.erase(topic);
        return dcms::kDcmsErrBusy;
    }

    // 在后台线程的订阅表中登记 lambda，收到消息时自动调用
    // cb_key = 函数指针的整数值，用于 unsubscribe 时匹配
    g_ctx.recv_thread->subscribe(
        topic,
        reinterpret_cast<std::uintptr_t>(cb), // 用函数指针值作为唯一键
        user,
        [cb, user](const dcms::DcmsMsgHeader *hdr, const void *payload,
                   std::size_t payload_len) {
            // 这个 lambda 在 RecvThread 线程里被调用
            cb(hdr->topic_id, payload, static_cast<std::uint32_t>(payload_len), user);
        });

    ++ref_count;
    return dcms::kDcmsOk;
}

// -----------------------------------------------------------------------------
// dcms_msg_unsubscribe: 取消订阅
//
// 引用计数减为 0 时，通知 IPC 层停止接收该 topic
// -----------------------------------------------------------------------------
int dcms_msg_unsubscribe(dcms_topic_id topic, dcms_msg_callback cb, void *user) {
    if (!cb)
        return dcms::kDcmsErrInvalidArg;

    std::lock_guard<std::mutex> lk(g_ctx_mutex);
    if (!g_ctx.inited)
        return dcms::kDcmsErrNotInit;

    g_ctx.recv_thread->unsubscribe(topic, reinterpret_cast<std::uintptr_t>(cb), user);

    auto it = g_ctx.topic_ref_counts.find(topic);
    if (it != g_ctx.topic_ref_counts.end()) {
        --it->second;
        if (it->second <= 0) {
            g_ctx.ipc->unsubscribeTopic(topic); // 最后一个订阅者退出，通知 IPC 层
            g_ctx.topic_ref_counts.erase(it);
        }
    }

    return dcms::kDcmsOk;
}

} // extern "C"
