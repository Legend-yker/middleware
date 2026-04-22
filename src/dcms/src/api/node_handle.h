#pragma once

#include "dcms_export.h"
#include "common/dcms_types.h"
#include "daemon/daemon_protocol.h"
#include "api/publisher.h"
#include "api/subscriber.h"
#include "api/service_server.h"
#include "api/service_client.h"

#include "utils/dcms_thread.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/shm.h>
#include <unistd.h>

namespace dcms {

class DCMS_API NodeHandle {
public:
    NodeHandle();
    ~NodeHandle();

    NodeHandle(NodeHandle &&) noexcept;
    NodeHandle &operator=(NodeHandle &&) noexcept;
    NodeHandle(const NodeHandle &)            = delete;
    NodeHandle &operator=(const NodeHandle &) = delete;

    // 连接 Daemon 并注册节点，成功返回 kDcmsOk。
    // 内部最多重试 3 次（间隔 100ms），处理 Daemon 启动比 Node 稍慢的竞争。
    static int create(const std::string &node_name, NodeHandle &out);

    bool valid() const { return daemon_fd_ >= 0; }

    // 声明发布 topic：向 Daemon 注册，取得 shm_name 后 mmap，返回可用的 Publisher。
    // T 必须是 trivially copyable（直接 memcpy 不能有虚表/资源句柄）。
    // 第一次调用时懒启动 notify_thread_（只有发布者才需要监听新订阅者加入通知）。
    template <typename T>
    Publisher advertise(const std::string &topic_name,
                        std::uint32_t ring_slots = kDefaultRingSlots) {
        return advertise_raw(topic_name,
                             static_cast<std::uint32_t>(sizeof(T)),
                             ring_slots);
    }

    // 运行时指定 slot_size 的非模板版本（SensorBase 等不知道具体 T 的场合使用）
    Publisher advertise_raw(const std::string &topic_name,
                            std::uint32_t slot_size,
                            std::uint32_t ring_slots = kDefaultRingSlots);

    // 订阅 topic：
    //   1. 向 Daemon 发 SubscribeReq
    //   2. 用 recvmsg（必须，非 recv）收 SubscribeAck + SCM_RIGHTS 传来的 eventfd
    //   3. shm_open 只读 mmap 共享内存（Subscriber 不能写）
    //   4. 构造 Subscriber，内部启动 recv_thread_ 监听 eventfd，有数据则回调 cb
    template <typename T>
    Subscriber<T> subscribe(const std::string &topic_name,
                            std::function<void(const T &)> cb) {
        SubscribeReq req{};
        std::strncpy(req.topic_name, topic_name.c_str(), kMaxTopicNameLen - 1);

        if (!sendMsg(DaemonMsgType::kSubscribe, &req, sizeof(req)))
            return Subscriber<T>{};

        DaemonMsgHeader hdr{};
        std::vector<std::uint8_t> payload;
        int eventfd_fd = -1;
        // 必须用 recvMsgWithFd（内部用 recvmsg），SCM_RIGHTS fd 只附在第一次 recvmsg 里
        if (!recvMsgWithFd(hdr, payload, eventfd_fd) ||
            hdr.type != DaemonMsgType::kSubscribeAck ||
            payload.size() < sizeof(SubscribeAck))
            return Subscriber<T>{};

        SubscribeAck ack{};
        std::memcpy(&ack, payload.data(), sizeof(ack));
        if (ack.status != kDcmsOk) {
            if (eventfd_fd >= 0) ::close(eventfd_fd);
            return Subscriber<T>{};
        }

        // 订阅者只读 mmap，防止意外修改共享数据
        int shmfd = ::shm_open(ack.shm_name, O_RDONLY, 0);
        if (shmfd < 0) { ::close(eventfd_fd); return Subscriber<T>{}; }

        void *ptr = ::mmap(nullptr, static_cast<std::size_t>(ack.shm_size),
                           PROT_READ, MAP_SHARED, shmfd, 0);
        ::close(shmfd); // fd 可立即关闭，mmap 持有独立引用
        if (ptr == MAP_FAILED) { ::close(eventfd_fd); return Subscriber<T>{}; }

        TopicId tid = static_cast<TopicId>(
            std::hash<std::string>{}(topic_name));
        return Subscriber<T>(tid, ptr, ack.shm_size, ack.ring_slots,
                             ack.slot_size, eventfd_fd, std::move(cb));
    }

    // 注册 RPC 服务端（不经过 Daemon，使用独立 Unix socket 点对点）
    template <typename Req, typename Resp>
    ServiceServer<Req, Resp> advertiseService(
        const std::string &service_name,
        typename ServiceServer<Req, Resp>::Handler handler) {
        return ServiceServer<Req, Resp>(service_name, std::move(handler));
    }

    // 连接 RPC 服务端
    template <typename Req, typename Resp>
    ServiceClient<Req, Resp> serviceClient(const std::string &service_name) {
        return ServiceClient<Req, Resp>(service_name);
    }

    // Publisher 构造/析构时调用，维护 topic_id → Publisher* 映射表，
    // 供 notifyLoop 在收到 kNewSubscriberNotify 时找到对应 Publisher 投递 eventfd
    int registerPublisher(TopicId id, Publisher *pub);
    int unregisterPublisher(TopicId id);

    int daemonFd() const { return daemon_fd_; }

private:
    // 监听 Daemon 发来的 kNewSubscriberNotify，把附带的 eventfd（SCM_RIGHTS）
    // 投递给 pub_registry_ 中对应的 Publisher::addSubscriberEventfd()。
    // 此线程在第一次 advertise 时懒启动，订阅者不需要它。
    void notifyLoop();

    bool sendMsg(DaemonMsgType type, const void *payload, std::size_t len);
    // 普通消息接收（不含 fd），内部用 recv 循环确保完整读取
    bool recvMsg(DaemonMsgHeader &hdr, std::vector<std::uint8_t> &payload,
                 int &ancfd);
    // 带 SCM_RIGHTS 的消息接收：头帧必须用 recvmsg 读，否则内核会丢弃附带的 fd
    bool recvMsgWithFd(DaemonMsgHeader &hdr, std::vector<std::uint8_t> &payload,
                       int &fd_out);
    bool recvExact(void *buf, std::size_t n);

    int daemon_fd_{-1};  // 与 Daemon 的 Unix stream socket 连接

    // notifyLoop 运行的线程（单次任务，析构时 wait）
    dcms::utils::SingleThread notify_thread_;
    std::atomic<bool>         notify_running_{false};

    std::mutex                               pub_registry_mutex_;
    std::unordered_map<TopicId, Publisher *> pub_registry_; // 不持有所有权
};

} // namespace dcms
