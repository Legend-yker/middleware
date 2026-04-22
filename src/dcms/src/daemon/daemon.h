#pragma once

#include "daemon/daemon_protocol.h"
#include "common/dcms_types.h"
#include "utils/dcms_thread.h"

#include <atomic>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace dcms {

// 每个已连接客户端（NodeHandle）的运行时信息
struct ClientInfo {
    int           fd;
    pid_t         pid;
    std::uint32_t node_id;
    std::string   node_name;
    std::unordered_set<TopicId> published_topics;  // 该节点 advertise 过的 topic
    std::unordered_set<TopicId> subscribed_topics; // 该节点 subscribe 过的 topic
};

// 每个 topic 的注册信息
struct TopicEntry {
    std::string   topic_name;
    TopicId       topic_id;
    std::string   shm_name;   // shm_open 名字，如 "/dcms_sensor_imu"
    std::uint64_t shm_size;
    std::uint32_t slot_size;
    std::uint32_t ring_slots;
    int           publisher_fd{-1};
    // Daemon 持有的 subscriber eventfd 的 dup 副本。
    // 用途：当 Publisher 重连时，Daemon 可以把已有的 eventfd 重新发给新的 Publisher。
    // Daemon 不用这些 fd 做 I/O，纯粹是为了保活（防止 eventfd 引用计数降零被关闭）。
    std::vector<int> subscriber_eventfds;
};

class Daemon {
public:
    Daemon();
    ~Daemon();

    Daemon(const Daemon &) = delete;
    Daemon &operator=(const Daemon &) = delete;

    // 绑定 Unix socket，创建 epoll，不启动线程。必须在 start() 之前调用。
    bool init();
    // 启动 acceptLoop 后台线程，返回 0 表示成功。
    int start();
    // 通知 acceptLoop 退出，等待线程 join，返回 0。
    int stop();

private:
    bool bindListenSocket();
    // epoll 主循环：accept 新连接 + 派发可读事件
    void acceptLoop();
    void handleClientReadable(int client_fd);
    void handleClientDisconnect(int client_fd);

    void handleNodeRegister(int client_fd, const NodeRegisterReq &req);
    // 创建 shm_open 区域，发回 shm_name 给 Publisher 自行 mmap
    void handleAdvertise(int client_fd, const AdvertiseReq &req);
    // 创建 eventfd，通过 SCM_RIGHTS 分别发给 Subscriber 和 Publisher
    void handleSubscribe(int client_fd, const SubscribeReq &req);

    bool createShmRegion(const std::string &shm_name, std::uint64_t size);

    static std::string topicToShmName(const std::string &topic_name);
    static TopicId     topicNameToId(const std::string &topic_name);

    // 发送一帧 PDU；ancillary_fd >= 0 时通过 SCM_RIGHTS 随消息传递文件描述符
    bool sendMsg(int fd, DaemonMsgType type, const void *payload,
                 std::size_t payload_len, int ancillary_fd = -1);

    // 精确读取 n 字节（处理流式套接字的短读）
    bool recvExact(int fd, void *buf, std::size_t n);

    // 接收一帧 PDU；ancillary_fd 接收 SCM_RIGHTS 传来的 fd（无则为 -1）
    bool recvMsg(int fd, DaemonMsgHeader &hdr,
                 std::vector<std::uint8_t> &payload, int &ancillary_fd);

    int  listen_fd_{-1};
    int  epoll_fd_{-1};
    std::atomic<bool>          running_{false};
    std::atomic<std::uint32_t> next_node_id_{1};
    dcms::utils::SingleThread  accept_thread_;

    std::mutex clients_mutex_;
    std::unordered_map<int /*fd*/, ClientInfo> clients_;

    std::mutex topics_mutex_;
    std::unordered_map<TopicId, TopicEntry>    topics_;
    std::unordered_map<std::string, TopicId>   topic_name_to_id_;
};

} // namespace dcms
