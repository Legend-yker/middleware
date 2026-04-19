#pragma once

#include "dcms_export.h"
#include "common/dcms_types.h"
#include "daemon/daemon_protocol.h"
#include "api/publisher.h"
#include "api/subscriber.h"
#include "api/service_server.h"
#include "api/service_client.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
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

    // Connect to the DCMS Daemon and register under node_name.
    // Returns kDcmsOk on success.
    static int create(const std::string &node_name, NodeHandle &out);

    bool valid() const { return daemon_fd_ >= 0; }

    // Advertise a topic: register with Daemon, mmap the ring buffer.
    // T must be trivially copyable.
    template <typename T>
    Publisher advertise(const std::string &topic_name,
                        std::uint32_t ring_slots = kDefaultRingSlots) {
        return advertise_raw(topic_name,
                             static_cast<std::uint32_t>(sizeof(T)),
                             ring_slots);
    }

    // Non-template variant: slot_size provided at runtime (e.g. from SensorBase).
    Publisher advertise_raw(const std::string &topic_name,
                            std::uint32_t slot_size,
                            std::uint32_t ring_slots = kDefaultRingSlots);

    // Subscribe to a topic: register with Daemon, mmap ring buffer, start recv thread.
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

        int shmfd = ::shm_open(ack.shm_name, O_RDONLY, 0);
        if (shmfd < 0) { ::close(eventfd_fd); return Subscriber<T>{}; }

        void *ptr = ::mmap(nullptr, static_cast<std::size_t>(ack.shm_size),
                           PROT_READ, MAP_SHARED, shmfd, 0);
        ::close(shmfd);
        if (ptr == MAP_FAILED) { ::close(eventfd_fd); return Subscriber<T>{}; }

        TopicId tid = static_cast<TopicId>(
            std::hash<std::string>{}(topic_name));
        return Subscriber<T>(tid, ptr, ack.shm_size, ack.ring_slots,
                             ack.slot_size, eventfd_fd, std::move(cb));
    }

    // Register a named RPC service (no Daemon involvement).
    template <typename Req, typename Resp>
    ServiceServer<Req, Resp> advertiseService(
        const std::string &service_name,
        typename ServiceServer<Req, Resp>::Handler handler) {
        return ServiceServer<Req, Resp>(service_name, std::move(handler));
    }

    // Connect to a named RPC service.
    template <typename Req, typename Resp>
    ServiceClient<Req, Resp> serviceClient(const std::string &service_name) {
        return ServiceClient<Req, Resp>(service_name);
    }

    // Called by Publisher ctor/dtor to maintain the pub_registry_
    void registerPublisher(TopicId id, Publisher *pub);
    void unregisterPublisher(TopicId id);

    int daemonFd() const { return daemon_fd_; }

private:
    void notifyLoop();

    bool sendMsg(DaemonMsgType type, const void *payload, std::size_t len);
    bool recvMsg(DaemonMsgHeader &hdr, std::vector<std::uint8_t> &payload,
                 int &ancfd);
    bool recvMsgWithFd(DaemonMsgHeader &hdr, std::vector<std::uint8_t> &payload,
                       int &fd_out);
    bool recvExact(void *buf, std::size_t n);

    int daemon_fd_{-1};

    std::thread       notify_thread_;
    std::atomic<bool> notify_running_{false};

    std::mutex                               pub_registry_mutex_;
    std::unordered_map<TopicId, Publisher *> pub_registry_;
};

} // namespace dcms
