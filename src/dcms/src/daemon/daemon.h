#pragma once

#include "daemon/daemon_protocol.h"
#include "common/dcms_types.h"

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace dcms {

struct ClientInfo {
    int           fd;
    pid_t         pid;
    std::uint32_t node_id;
    std::string   node_name;
    std::unordered_set<TopicId> published_topics;
    std::unordered_set<TopicId> subscribed_topics;
};

struct TopicEntry {
    std::string   topic_name;
    TopicId       topic_id;
    std::string   shm_name;
    std::uint64_t shm_size;
    std::uint32_t slot_size;
    std::uint32_t ring_slots;
    int           publisher_fd{-1};
    // Daemon-owned dup copies of subscriber eventfds (for publisher reconnect)
    std::vector<int> subscriber_eventfds;
};

class Daemon {
public:
    Daemon();
    ~Daemon();

    Daemon(const Daemon &) = delete;
    Daemon &operator=(const Daemon &) = delete;

    // Block until stop() is called or fatal error. Returns exit code.
    int  run();
    void stop();

private:
    bool bindListenSocket();
    void acceptLoop();
    void handleClientReadable(int client_fd);
    void handleClientDisconnect(int client_fd);

    void handleNodeRegister(int client_fd, const NodeRegisterReq &req);
    void handleAdvertise(int client_fd, const AdvertiseReq &req);
    void handleSubscribe(int client_fd, const SubscribeReq &req);

    bool createShmRegion(const std::string &shm_name, std::uint64_t size);

    static std::string topicToShmName(const std::string &topic_name);
    static TopicId     topicNameToId(const std::string &topic_name);

    // Send a framed PDU; if ancillary_fd >= 0 it is passed via SCM_RIGHTS
    bool sendMsg(int fd, DaemonMsgType type, const void *payload,
                 std::size_t payload_len, int ancillary_fd = -1);

    // Receive exactly n bytes (handles short reads on stream sockets)
    bool recvExact(int fd, void *buf, std::size_t n);

    // Receive one framed PDU; ancillary_fd receives SCM_RIGHTS fd (-1 if none)
    bool recvMsg(int fd, DaemonMsgHeader &hdr,
                 std::vector<std::uint8_t> &payload, int &ancillary_fd);

    int  listen_fd_{-1};
    int  epoll_fd_{-1};
    std::atomic<bool>          running_{false};
    std::atomic<std::uint32_t> next_node_id_{1};

    std::mutex clients_mutex_;
    std::unordered_map<int /*fd*/, ClientInfo> clients_;

    std::mutex topics_mutex_;
    std::unordered_map<TopicId, TopicEntry>    topics_;
    std::unordered_map<std::string, TopicId>   topic_name_to_id_;
};

} // namespace dcms
