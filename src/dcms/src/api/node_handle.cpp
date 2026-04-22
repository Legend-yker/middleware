#include "api/node_handle.h"

#include <cerrno>
#include <chrono>
#include <cstring>

#include <fcntl.h>
#include <poll.h>
#include <sys/uio.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace dcms {

NodeHandle::NodeHandle()  = default;

NodeHandle::~NodeHandle() {
    notify_running_ = false;
    notify_thread_.wait();
    if (daemon_fd_ >= 0) {
        ::close(daemon_fd_);
        daemon_fd_ = -1;
    }
}

NodeHandle::NodeHandle(NodeHandle &&o) noexcept
    : daemon_fd_(o.daemon_fd_),
      notify_thread_(std::move(o.notify_thread_)),
      pub_registry_(std::move(o.pub_registry_)) {
    notify_running_.store(o.notify_running_.load());
    o.daemon_fd_      = -1;
    o.notify_running_ = false;
}

NodeHandle &NodeHandle::operator=(NodeHandle &&o) noexcept {
    if (this == &o) return *this;
    this->~NodeHandle();
    new (this) NodeHandle(std::move(o));
    return *this;
}

int NodeHandle::create(const std::string &node_name, NodeHandle &out) {
    constexpr int kMaxRetry   = 3;
    constexpr int kRetryDelay = 100; // ms

    int fd = -1;
    for (int attempt = 0; attempt < kMaxRetry; ++attempt) {
        int s = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (s < 0) return kDcmsErrNotImpl;

        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, kDaemonSocketPath,
                     sizeof(addr.sun_path) - 1);

        if (::connect(s, reinterpret_cast<sockaddr *>(&addr),
                      sizeof(addr)) == 0) {
            fd = s;
            break;
        }
        ::close(s);
        if (attempt + 1 < kMaxRetry)
            std::this_thread::sleep_for(std::chrono::milliseconds(kRetryDelay));
    }

    if (fd < 0)
        return kDcmsErrDaemonNotFound;

    // Build the NodeHandle with the connected fd, but don't start the thread yet
    NodeHandle nh;
    nh.daemon_fd_ = fd;

    // Register with Daemon
    NodeRegisterReq req{};
    std::strncpy(req.node_name, node_name.c_str(), kMaxNodeNameLen - 1);
    req.pid = ::getpid();

    if (!nh.sendMsg(DaemonMsgType::kNodeRegister, &req, sizeof(req))) {
        return kDcmsErrNotImpl;
    }

    DaemonMsgHeader hdr{};
    std::vector<std::uint8_t> payload;
    int ancfd = -1;
    if (!nh.recvMsg(hdr, payload, ancfd) ||
        hdr.type != DaemonMsgType::kNodeRegisterAck ||
        payload.size() < sizeof(NodeRegisterAck)) {
        return kDcmsErrNotImpl;
    }

    NodeRegisterAck ack{};
    std::memcpy(&ack, payload.data(), sizeof(ack));
    if (ack.status != kDcmsOk)
        return static_cast<int>(ack.status);

    // Move into out first. notify_thread_ is started lazily in advertise<T>()
    // so subscriber-only nodes don't have a thread competing for daemon_fd_.
    out = std::move(nh);

    return kDcmsOk;
}

void NodeHandle::notifyLoop() {
    while (notify_running_.load(std::memory_order_relaxed)) {
        pollfd pfd{daemon_fd_, POLLIN, 0};
        // 100ms 超时轮询，确保 notify_running_=false 后能及时退出
        int rc = ::poll(&pfd, 1, 100 /*ms*/);
        if (rc <= 0) continue;
        if (pfd.revents & (POLLHUP | POLLERR)) {
            notify_running_ = false;
            break;
        }
        if (!(pfd.revents & POLLIN)) continue;

        DaemonMsgHeader hdr{};
        std::vector<std::uint8_t> payload;
        int efd = -1;
        if (!recvMsgWithFd(hdr, payload, efd))
            break;

        if (hdr.type == DaemonMsgType::kNewSubscriberNotify && efd >= 0) {
            // 有新 Subscriber 加入：把 Daemon 通过 SCM_RIGHTS 传来的 eventfd
            // 投递给本进程所有 Publisher（通常一个 topic 只有一个 Publisher）。
            // 用 dup 是因为每个 Publisher 需要独立的 fd，close 时互不影响。
            std::lock_guard<std::mutex> lk(pub_registry_mutex_);
            for (auto &[tid, pub] : pub_registry_) {
                if (pub)
                    pub->addSubscriberEventfd(::dup(efd));
            }
            ::close(efd); // 原 fd 已 dup 给各 Publisher，自身关闭
        }
    }
}

Publisher NodeHandle::advertise_raw(const std::string &topic_name,
                                    std::uint32_t slot_size,
                                    std::uint32_t ring_slots) {
    AdvertiseReq req{};
    std::strncpy(req.topic_name, topic_name.c_str(), kMaxTopicNameLen - 1);
    req.slot_size  = slot_size;
    req.ring_slots = ring_slots;

    if (!sendMsg(DaemonMsgType::kAdvertise, &req, sizeof(req)))
        return Publisher{};

    DaemonMsgHeader hdr{};
    std::vector<std::uint8_t> payload;
    int ancfd = -1;
    if (!recvMsg(hdr, payload, ancfd) ||
        hdr.type != DaemonMsgType::kAdvertiseAck ||
        payload.size() < sizeof(AdvertiseAck))
        return Publisher{};

    AdvertiseAck ack{};
    std::memcpy(&ack, payload.data(), sizeof(ack));
    if (ack.status != kDcmsOk)
        return Publisher{};

    int shmfd = ::shm_open(ack.shm_name, O_RDWR, 0);
    if (shmfd < 0) return Publisher{};

    void *ptr = ::mmap(nullptr, static_cast<std::size_t>(ack.shm_size),
                       PROT_READ | PROT_WRITE, MAP_SHARED, shmfd, 0);
    ::close(shmfd);
    if (ptr == MAP_FAILED) return Publisher{};

    TopicId tid = static_cast<TopicId>(
        std::hash<std::string>{}(topic_name));

    bool expected = false;
    if (notify_running_.compare_exchange_strong(expected, true,
            std::memory_order_acquire, std::memory_order_relaxed)) {
        notify_thread_ = dcms::utils::SingleThread(
            "dcms_notify", [this] { notifyLoop(); });
    }

    return Publisher(tid, ptr, ack.shm_size, ring_slots, slot_size, this);
}

int NodeHandle::registerPublisher(TopicId id, Publisher *pub) {
    std::lock_guard<std::mutex> lk(pub_registry_mutex_);
    pub_registry_[id] = pub;
    return 0;
}

int NodeHandle::unregisterPublisher(TopicId id) {
    std::lock_guard<std::mutex> lk(pub_registry_mutex_);
    pub_registry_.erase(id);
    return 0;
}

bool NodeHandle::sendMsg(DaemonMsgType type, const void *payload,
                         std::size_t len) {
    DaemonMsgHeader hdr{type, static_cast<std::uint32_t>(len)};
    iovec iov[2];
    int   niov = 1;
    iov[0].iov_base = &hdr;
    iov[0].iov_len  = sizeof(hdr);
    if (payload && len > 0) {
        iov[1].iov_base = const_cast<void *>(payload);
        iov[1].iov_len  = len;
        niov = 2;
    }
    return ::writev(daemon_fd_, iov, niov) >= 0;
}

bool NodeHandle::recvExact(void *buf, std::size_t n) {
    std::size_t got = 0;
    while (got < n) {
        ssize_t r = ::recv(daemon_fd_, static_cast<char *>(buf) + got,
                           n - got, 0);
        if (r <= 0) return false;
        got += static_cast<std::size_t>(r);
    }
    return true;
}

bool NodeHandle::recvMsg(DaemonMsgHeader &hdr,
                         std::vector<std::uint8_t> &payload, int &ancfd) {
    ancfd = -1;
    if (!recvExact(&hdr, sizeof(hdr))) return false;
    payload.clear();
    if (hdr.payload_len > 0) {
        payload.resize(hdr.payload_len);
        if (!recvExact(payload.data(), hdr.payload_len)) return false;
    }
    return true;
}

bool NodeHandle::recvMsgWithFd(DaemonMsgHeader &hdr,
                                std::vector<std::uint8_t> &payload,
                                int &fd_out) {
    fd_out = -1;

    // SCM_RIGHTS 的关键约束：ancillary data（附带的 fd）与 sendmsg() 发出的
    // 第一个字节绑定。如果用普通 recv() 读走了那个字节，内核就会丢弃 fd。
    // 因此帧头必须用 recvmsg() 读，同时提供 cmsg 缓冲区来捕获 fd。
    // Payload 字节在帧头之后，此时 SCM_RIGHTS 已经取出，后续可以用普通 recv。
    iovec iov{&hdr, sizeof(hdr)};
    union { char buf[CMSG_SPACE(sizeof(int))]; cmsghdr align; } ctrl{};
    msghdr msg{};
    msg.msg_iov        = &iov;
    msg.msg_iovlen     = 1;
    msg.msg_control    = ctrl.buf;
    msg.msg_controllen = sizeof(ctrl.buf);

    ssize_t r = ::recvmsg(daemon_fd_, &msg, MSG_WAITALL);
    if (r != static_cast<ssize_t>(sizeof(hdr)))
        return false;

    // 从 cmsg 控制消息中取出传递过来的文件描述符
    cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    if (cmsg && cmsg->cmsg_type == SCM_RIGHTS)
        std::memcpy(&fd_out, CMSG_DATA(cmsg), sizeof(int));

    // SCM_RIGHTS 已经取出，后续 payload 可以用普通 recv 读取
    payload.clear();
    if (hdr.payload_len > 0) {
        payload.resize(hdr.payload_len);
        if (!recvExact(payload.data(), hdr.payload_len)) return false;
    }
    return true;
}

} // namespace dcms
