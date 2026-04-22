#include "daemon/daemon.h"
#include "shm/ring_buffer.h"

#include "dlog/dlog.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <functional>

#define DAEMON_LOGI(msg) DLOG_INFO("[MOD:dcms_daemon]", msg)
#define DAEMON_LOGW(msg) DLOG_WARN("[MOD:dcms_daemon]", msg)
#define DAEMON_LOGE(msg) DLOG_ERROR("[MOD:dcms_daemon]", msg)

#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/uio.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace dcms {

// ---------------------------------------------------------------------------
// helpers: sendmsg with optional SCM_RIGHTS fd
// ---------------------------------------------------------------------------
static bool sendFd(int sock, int fd_to_send) {
    char  dummy  = 0;
    iovec iov    = {&dummy, 1};

    union {
        char           buf[CMSG_SPACE(sizeof(int))];
        cmsghdr        align;
    } ctrl{};

    msghdr msg{};
    msg.msg_iov        = &iov;
    msg.msg_iovlen     = 1;
    msg.msg_control    = ctrl.buf;
    msg.msg_controllen = sizeof(ctrl.buf);

    cmsghdr *cmsg      = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level   = SOL_SOCKET;
    cmsg->cmsg_type    = SCM_RIGHTS;
    cmsg->cmsg_len     = CMSG_LEN(sizeof(int));
    std::memcpy(CMSG_DATA(cmsg), &fd_to_send, sizeof(int));

    return sendmsg(sock, &msg, MSG_NOSIGNAL) >= 0;
}

static int recvFd(int sock) {
    char  dummy = 0;
    iovec iov   = {&dummy, 1};

    union {
        char    buf[CMSG_SPACE(sizeof(int))];
        cmsghdr align;
    } ctrl{};

    msghdr msg{};
    msg.msg_iov        = &iov;
    msg.msg_iovlen     = 1;
    msg.msg_control    = ctrl.buf;
    msg.msg_controllen = sizeof(ctrl.buf);

    if (recvmsg(sock, &msg, 0) <= 0)
        return -1;

    cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    if (!cmsg || cmsg->cmsg_type != SCM_RIGHTS)
        return -1;

    int fd = -1;
    std::memcpy(&fd, CMSG_DATA(cmsg), sizeof(int));
    return fd;
}

// ---------------------------------------------------------------------------
// Daemon
// ---------------------------------------------------------------------------
Daemon::Daemon()  = default;
Daemon::~Daemon() {
    stop();
    if (epoll_fd_ >= 0) { ::close(epoll_fd_); epoll_fd_ = -1; }
    if (listen_fd_ >= 0) { ::close(listen_fd_); listen_fd_ = -1; }
    ::unlink(kDaemonSocketPath);
}

bool Daemon::init() {
    if (!bindListenSocket())
        return false;

    epoll_fd_ = ::epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd_ < 0) {
        std::perror("epoll_create1");
        return false;
    }
    //这里为何要
    epoll_event ev{};
    ev.events  = EPOLLIN;
    ev.data.fd = listen_fd_;
    if (::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, listen_fd_, &ev) < 0) {
        std::perror("epoll_ctl listen_fd");
        return false;
    }
    return true;
}

int Daemon::start() {
    running_       = true;
    accept_thread_ = dcms::utils::SingleThread(
        "dcms_accept", [this] { acceptLoop(); });
    return 0;
}

int Daemon::stop() {
    running_ = false;
    accept_thread_.wait();
    return 0;
}

bool Daemon::bindListenSocket() {
    ::unlink(kDaemonSocketPath);

    listen_fd_ = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (listen_fd_ < 0) {
        std::perror("socket");
        return false;
    }

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, kDaemonSocketPath, sizeof(addr.sun_path) - 1);

    if (::bind(listen_fd_, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
        std::perror("bind");
        return false;
    }

    ::chmod(kDaemonSocketPath, 0666);

    if (::listen(listen_fd_, 16) < 0) {
        std::perror("listen");
        return false;
    }
    return true;
}

void Daemon::acceptLoop() {
    constexpr int kMaxEvents = 64;
    epoll_event   events[kMaxEvents]; // 每次 epoll_wait 最多返回 64 个就绪事件

    // running_ 由 stop() 置 false，循环最多再等 200ms 就会退出
    while (running_.load(std::memory_order_relaxed)) {

        // 阻塞等待，最多 200ms；超时返回 0，有事件返回事件数，出错返回 -1
        int n = ::epoll_wait(epoll_fd_, events, kMaxEvents, 200 /*ms*/);
        if (n < 0) {
            if (errno == EINTR) continue; // 被信号打断，不是真正出错，重试
            std::perror("epoll_wait: no event happend");
            break;
        }

        // 遍历本轮所有就绪的 fd
        for (int i = 0; i < n; ++i) {
            int fd = events[i].data.fd; // 取出就绪的 fd（init 时通过 ev.data.fd 存入）

            if (fd == listen_fd_) {
                // ── 情况1：监听 socket 可读 → 有新进程来连接 ──────────────────
                // accept4 比 accept 多两个 flag：
                //   SOCK_CLOEXEC  : fork 后子进程不继承此 fd
                //   SOCK_NONBLOCK : 新连接设为非阻塞，配合 EPOLLET 使用
                int cfd = ::accept4(listen_fd_, nullptr, nullptr,
                                    SOCK_CLOEXEC | SOCK_NONBLOCK);
                if (cfd < 0) continue; // accept 失败（如系统 fd 耗尽），跳过

                // 把新连接加入 epoll 监听
                epoll_event cev{};
                cev.events  = EPOLLIN   // 有数据可读（NodeHandle 发来控制消息）
                            | EPOLLET   // 边缘触发：只通知一次，必须在 handleClientReadable 里把数据读干净
                            | EPOLLHUP  // 对端关闭连接
                            | EPOLLERR; // socket 出错
                cev.data.fd = cfd;      // 事件触发时通过 events[i].data.fd 找回这个 fd
                ::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, cfd, &cev);

                // 记录客户端信息（pid/node_id 等待后续 NodeRegisterReq 填写）
                std::lock_guard<std::mutex> lk(clients_mutex_);
                clients_[cfd] = ClientInfo{cfd, 0, 0, ""};

            } else if (events[i].events & (EPOLLHUP | EPOLLERR)) {
                // ── 情况2：客户端断开或出错 ────────────────────────────────────
                // 清理该客户端发布的 topic（shm_unlink）和持有的 eventfd dup 副本
                handleClientDisconnect(fd);

            } else if (events[i].events & EPOLLIN) {
                // ── 情况3：客户端发来控制消息（Register / Advertise / Subscribe）
                // 内部循环读完所有积压消息（EPOLLET 边缘触发要求必须读干净）
                handleClientReadable(fd);
            }
        }
    }
}

void Daemon::handleClientReadable(int client_fd) {
    // EPOLLET 边缘触发：内核只通知一次"有新数据到了"。
    // 如果这次没读完，剩余数据不会再触发事件，必须循环读干净。
    while (true) {
        // ── Step1：读取一帧 PDU（帧头 + payload）─────────────────────────────
        // 每条消息格式：[DaemonMsgHeader 8B][payload N B]
        // recvMsg 内部先读固定长度帧头，再按 payload_len 继续读 payload
        DaemonMsgHeader           hdr{};
        std::vector<std::uint8_t> payload;
        int ancfd = -1; // 此路径不收 SCM_RIGHTS fd（只有订阅回包才有，由 sendMsg 发出）

        if (!recvMsg(client_fd, hdr, payload, ancfd)) {
            // 读失败 = 对端已关闭连接或 socket 出错
            // 主动清理：shm_unlink、关闭 eventfd dup 副本、从 epoll 移除
            handleClientDisconnect(client_fd);
            return;
        }

        // ── Step2：按消息类型分发处理 ─────────────────────────────────────────
        // hdr.type 由客户端（NodeHandle）发送时填写，对应四阶段协议的请求类型
        switch (hdr.type) {

        case DaemonMsgType::kNodeRegister:
            // 阶段一：NodeHandle::create() 连接后的第一条消息，内容是 node_name + pid
            // 处理：分配 node_id，写入 clients_ 表，回复 NodeRegisterAck
            if (payload.size() >= sizeof(NodeRegisterReq)) {
                NodeRegisterReq req{};
                std::memcpy(&req, payload.data(), sizeof(req)); // vector → 结构体反序列化
                handleNodeRegister(client_fd, req);
            }
            break;

        case DaemonMsgType::kAdvertise:
            // 阶段二：NodeHandle::advertise_raw() 发来的发布声明，内容是 topic_name + slot_size + ring_slots
            // 处理：shm_open 创建共享内存并初始化 RingHeader，回复 AdvertiseAck（含 shm_name）
            //       Publisher 拿到 shm_name 后自己 mmap，Daemon 不再持有该内存映射
            if (payload.size() >= sizeof(AdvertiseReq)) {
                AdvertiseReq req{};
                std::memcpy(&req, payload.data(), sizeof(req));
                handleAdvertise(client_fd, req);
            }
            break;

        case DaemonMsgType::kSubscribe:
            // 阶段三：NodeHandle::subscribe<T>() 发来的订阅请求，内容是 topic_name
            // 处理流程：
            //   1. 查找 topic 对应的 shm 信息
            //   2. eventfd() 创建一个通知用的 fd（计数器，初值为0）
            //   3. dup 出两个副本：一个给 Publisher，一个 Daemon 自己保留
            //   4. 原 efd 通过 SCM_RIGHTS 附在 SubscribeAck 里发给 Subscriber
            //   5. dup 副本通过 SCM_RIGHTS 附在 kNewSubscriberNotify 里发给 Publisher
            //   → 之后 Publisher write(efd,1) 直接唤醒 Subscriber，Daemon 不再参与
            if (payload.size() >= sizeof(SubscribeReq)) {
                SubscribeReq req{};
                std::memcpy(&req, payload.data(), sizeof(req));
                handleSubscribe(client_fd, req);
            }
            break;

        default:
            break;
        }

        // ── Step3：检查缓冲区是否还有剩余数据（EPOLLET 必须读干净）────────────
        // MSG_PEEK    : 偷看 1 字节但不消耗，数据仍在缓冲区
        // MSG_DONTWAIT: 非阻塞，缓冲区空时立即返回 EAGAIN（<=0）
        // 返回 <=0 说明本次数据已读完，退出循环，等下次 epoll 事件
        char peek = 0;
        if (::recv(client_fd, &peek, 1, MSG_PEEK | MSG_DONTWAIT) <= 0)
            break;
    }
}

void Daemon::handleClientDisconnect(int client_fd) {
    epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, client_fd, nullptr);

    std::lock_guard<std::mutex> lk_c(clients_mutex_);
    auto it = clients_.find(client_fd);
    if (it == clients_.end()) {
        ::close(client_fd);
        return;
    }
    ClientInfo info = std::move(it->second);
    clients_.erase(it);
    ::close(client_fd);
    DAEMON_LOGI("client_disconnect: name=" + info.node_name +
                " pid=" + std::to_string(info.pid));

    std::lock_guard<std::mutex> lk_t(topics_mutex_);

    // Clean up published topics
    for (TopicId tid : info.published_topics) {
        auto tt = topics_.find(tid);
        if (tt == topics_.end()) continue;
        TopicEntry &entry = tt->second;
        ::shm_unlink(entry.shm_name.c_str());
        for (int efd : entry.subscriber_eventfds)
            ::close(efd);
        // Remove name mapping
        topic_name_to_id_.erase(entry.topic_name);
        topics_.erase(tt);
    }

    // Clean up subscribed topics (remove this subscriber's eventfd dup)
    for (TopicId tid : info.subscribed_topics) {
        auto tt = topics_.find(tid);
        if (tt == topics_.end()) continue;
        // We stored one dup per subscriber in subscriber_eventfds;
        // without a per-fd index we close the last one as approximation.
        // A production impl would store (subscriber_fd → eventfd) pairs.
        auto &efds = tt->second.subscriber_eventfds;
        if (!efds.empty()) {
            ::close(efds.back());
            efds.pop_back();
        }
    }
}

void Daemon::handleNodeRegister(int client_fd, const NodeRegisterReq &req) {
    std::uint32_t node_id = next_node_id_.fetch_add(1u, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lk(clients_mutex_);
        auto it = clients_.find(client_fd);
        if (it != clients_.end()) {
            it->second.node_id   = node_id;
            it->second.pid       = req.pid;
            it->second.node_name = req.node_name;
        }
    }
    DAEMON_LOGI("node_register: name=" + std::string(req.node_name) +
                " pid=" + std::to_string(req.pid) +
                " node_id=" + std::to_string(node_id));
    NodeRegisterAck ack{node_id, kDcmsOk};
    sendMsg(client_fd, DaemonMsgType::kNodeRegisterAck, &ack, sizeof(ack));
}

void Daemon::handleAdvertise(int client_fd, const AdvertiseReq &req) {
    if (req.slot_size == 0 || req.ring_slots == 0 || req.ring_slots > 1024) {
        ErrorResp err{kDcmsErrInvalidArg, "invalid slot_size or ring_slots"};
        sendMsg(client_fd, DaemonMsgType::kError, &err, sizeof(err));
        return;
    }

    TopicId      tid      = topicNameToId(req.topic_name);
    std::string  shm_name = topicToShmName(req.topic_name);
    std::uint64_t shm_size =
        pageCeil(ringBufferSize(req.ring_slots, req.slot_size));

    {
        std::lock_guard<std::mutex> lk(topics_mutex_);
        auto it = topics_.find(tid);
        if (it != topics_.end() && it->second.publisher_fd >= 0) {
            ErrorResp err{kDcmsErrTopicExists, "topic already has a publisher"};
            sendMsg(client_fd, DaemonMsgType::kError, &err, sizeof(err));
            return;
        }
        // Recreate if stale entry exists
        if (it != topics_.end()) {
            ::shm_unlink(it->second.shm_name.c_str());
            topics_.erase(it);
        }
    }

    if (!createShmRegion(shm_name, shm_size)) {
        ErrorResp err{kDcmsErrShmFail, "shm_open/ftruncate failed"};
        sendMsg(client_fd, DaemonMsgType::kError, &err, sizeof(err));
        return;
    }

    {
        std::lock_guard<std::mutex> lk_t(topics_mutex_);
        TopicEntry entry{};
        entry.topic_name   = req.topic_name;
        entry.topic_id     = tid;
        entry.shm_name     = shm_name;
        entry.shm_size     = shm_size;
        entry.slot_size    = req.slot_size;
        entry.ring_slots   = req.ring_slots;
        entry.publisher_fd = client_fd;
        topics_[tid]            = std::move(entry);
        topic_name_to_id_[req.topic_name] = tid;
    }
    {
        std::lock_guard<std::mutex> lk(clients_mutex_);
        auto it = clients_.find(client_fd);
        if (it != clients_.end())
            it->second.published_topics.insert(tid);
    }

    // Initialize RingHeader in the shm region so subscribers see valid metadata
    {
        int fd = ::shm_open(shm_name.c_str(), O_RDWR, 0);
        if (fd >= 0) {
            void *ptr = ::mmap(nullptr, shm_size, PROT_READ | PROT_WRITE,
                               MAP_SHARED, fd, 0);
            ::close(fd);
            if (ptr != MAP_FAILED) {
                auto *rhdr      = reinterpret_cast<RingHeader *>(ptr);
                rhdr->ring_slots = req.ring_slots;
                rhdr->slot_size  = req.slot_size;
                // write_idx already 0 from ftruncate zero-fill
                ::munmap(ptr, shm_size);
            }
        }
    }

    DAEMON_LOGI("advertise: topic=" + std::string(req.topic_name) +
                " shm=" + shm_name +
                " slots=" + std::to_string(req.ring_slots) +
                " slot_size=" + std::to_string(req.slot_size));
    AdvertiseAck ack{};
    std::strncpy(ack.shm_name, shm_name.c_str(), kMaxShmNameLen - 1);
    ack.shm_size = shm_size;
    ack.status   = kDcmsOk;
    sendMsg(client_fd, DaemonMsgType::kAdvertiseAck, &ack, sizeof(ack));
}

void Daemon::handleSubscribe(int client_fd, const SubscribeReq &req) {
    TopicId tid = topicNameToId(req.topic_name);

    std::unique_lock<std::mutex> lk(topics_mutex_);
    auto it = topics_.find(tid);
    if (it == topics_.end()) {
        lk.unlock();
        ErrorResp err{kDcmsErrNotImpl, "topic not advertised"};
        sendMsg(client_fd, DaemonMsgType::kError, &err, sizeof(err));
        return;
    }
    TopicEntry &entry = it->second;

    // Create eventfd for this subscriber
    int efd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (efd < 0) {
        lk.unlock();
        ErrorResp err{kDcmsErrShmFail, "eventfd failed"};
        sendMsg(client_fd, DaemonMsgType::kError, &err, sizeof(err));
        return;
    }

    int dup_for_daemon     = ::dup(efd);   // Daemon retains for publisher reconnect
    int dup_for_publisher  = ::dup(efd);   // Will be sent to publisher

    entry.subscriber_eventfds.push_back(dup_for_daemon);

    SubscribeAck ack{};
    std::strncpy(ack.shm_name, entry.shm_name.c_str(), kMaxShmNameLen - 1);
    ack.shm_size  = entry.shm_size;
    ack.slot_size = entry.slot_size;
    ack.ring_slots= entry.ring_slots;
    ack.status    = kDcmsOk;

    int publisher_fd = entry.publisher_fd;
    lk.unlock();

    // Send SubscribeAck + efd (original) to subscriber via SCM_RIGHTS
    // We must send efd before closing it. sendMsg handles ancillary.
    sendMsg(client_fd, DaemonMsgType::kSubscribeAck, &ack, sizeof(ack), efd);
    ::close(efd);  // subscriber now owns its copy

    // Notify publisher of new subscriber, pass dup_for_publisher via SCM_RIGHTS
    if (publisher_fd >= 0) {
        sendMsg(publisher_fd, DaemonMsgType::kNewSubscriberNotify,
                nullptr, 0, dup_for_publisher);
    }
    ::close(dup_for_publisher);

    {
        std::lock_guard<std::mutex> lk_c(clients_mutex_);
        auto cit = clients_.find(client_fd);
        if (cit != clients_.end())
            cit->second.subscribed_topics.insert(tid);
    }
    DAEMON_LOGI("subscribe: topic=" + std::string(req.topic_name) +
                " has_publisher=" + (publisher_fd >= 0 ? "yes" : "no"));
}

bool Daemon::createShmRegion(const std::string &shm_name, std::uint64_t size) {
    // Unlink first in case a previous run left it
    ::shm_unlink(shm_name.c_str());

    int fd = ::shm_open(shm_name.c_str(), O_CREAT | O_RDWR | O_TRUNC, 0600);
    if (fd < 0) {
        std::perror("shm_open");
        return false;
    }
    if (::ftruncate(fd, static_cast<off_t>(size)) < 0) {
        std::perror("ftruncate");
        ::close(fd);
        ::shm_unlink(shm_name.c_str());
        return false;
    }
    ::close(fd);   // Daemon does NOT mmap; clients do their own mmap
    return true;
}

std::string Daemon::topicToShmName(const std::string &topic_name) {
    std::string name = "/dcms_";
    for (char c : topic_name)
        name += (c == '/') ? '_' : c;
    if (name.size() >= kMaxShmNameLen)
        name.resize(kMaxShmNameLen - 1);
    return name;
}

TopicId Daemon::topicNameToId(const std::string &topic_name) {
    return static_cast<TopicId>(std::hash<std::string>{}(topic_name));
}

bool Daemon::sendMsg(int fd, DaemonMsgType type, const void *payload,
                     std::size_t payload_len, int ancillary_fd) {
    DaemonMsgHeader hdr{type, static_cast<std::uint32_t>(payload_len)};

    if (ancillary_fd < 0) {
        // Simple writev: header + optional payload
        iovec iov[2];
        int   niov = 1;
        iov[0].iov_base = &hdr;
        iov[0].iov_len  = sizeof(hdr);
        if (payload && payload_len > 0) {
            iov[1].iov_base = const_cast<void *>(payload);
            iov[1].iov_len  = payload_len;
            niov = 2;
        }
        return ::writev(fd, iov, niov) >= 0;
    }

    // sendmsg with SCM_RIGHTS
    iovec iov[2];
    int   niov = 1;
    iov[0].iov_base = &hdr;
    iov[0].iov_len  = sizeof(hdr);
    if (payload && payload_len > 0) {
        iov[1].iov_base = const_cast<void *>(payload);
        iov[1].iov_len  = payload_len;
        niov = 2;
    }

    union { char buf[CMSG_SPACE(sizeof(int))]; cmsghdr align; } ctrl{};

    msghdr msg{};
    msg.msg_iov        = iov;
    msg.msg_iovlen     = niov;
    msg.msg_control    = ctrl.buf;
    msg.msg_controllen = sizeof(ctrl.buf);

    cmsghdr *cmsg    = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type  = SCM_RIGHTS;
    cmsg->cmsg_len   = CMSG_LEN(sizeof(int));
    std::memcpy(CMSG_DATA(cmsg), &ancillary_fd, sizeof(int));

    return ::sendmsg(fd, &msg, MSG_NOSIGNAL) >= 0;
}

bool Daemon::recvExact(int fd, void *buf, std::size_t n) {
    std::size_t got = 0;
    while (got < n) {
        ssize_t r = ::recv(fd, static_cast<char *>(buf) + got, n - got, 0);
        if (r <= 0) return false;
        got += static_cast<std::size_t>(r);
    }
    return true;
}

bool Daemon::recvMsg(int fd, DaemonMsgHeader &hdr,
                     std::vector<std::uint8_t> &payload, int &ancillary_fd) {
    ancillary_fd = -1;
    if (!recvExact(fd, &hdr, sizeof(hdr)))
        return false;
    payload.clear();
    if (hdr.payload_len > 0) {
        payload.resize(hdr.payload_len);
        if (!recvExact(fd, payload.data(), hdr.payload_len))
            return false;
    }
    return true;
}

} // namespace dcms
