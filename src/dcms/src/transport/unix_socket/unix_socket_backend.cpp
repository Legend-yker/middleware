// =============================================================================
// unix_socket_backend.cpp —— Linux 平台 IPC 后端
//
// 这个类实现了两种完全不同的工作模式：
//
// 【模式 A：socketpair（同进程，默认）】
//   用途：同一个进程内的发布者和订阅者通信
//   原理：创建一对互连的 UNIX 套接字 (fds[0] <→ fds[1])
//         发送方把"SHM 指针 + 长度"写入 fds[0]
//         接收方从 fds[1] 读出指针，直接访问内存（零拷贝！）
//
//   地址空间：                 共享内存（ShmPool）
//   ┌──────────┐               ┌──────────────────┐
//   │ 发布方   │──写入指针────→│  [Header][Data]  │
//   └──────────┘               └──────────────────┘
//   socketpair 只传 8 字节              ↑
//   ┌──────────┐               接收方直接访问同一块内存
//   │ 接收方   │──读出指针────→│（因为同进程地址相同）│
//   └──────────┘               └──────────────────┘
//
// 【模式 B：Unix Socket Broker（多进程）】
//   用途：不同进程间通信（设置环境变量 DCMS_USE_BROKER=1 启用）
//   原理：有一个独立的 broker 进程（dcms_app）充当消息路由中心
//         发布方把完整消息序列化后发给 broker
//         broker 查找订阅者，转发给各个客户端
//         每个客户端有自己的 Unix Datagram Socket 文件
//
//   进程A (发布方)                 broker 进程                进程B (订阅方)
//   ─────────────                 ────────────               ─────────────
//   发送 BrokerPacket        →    接收、查订阅表       →    接收完整消息
//   [BrokerPacketHeader]          找到 topic 的所有订阅者     写入 ShmPool
//   [完整 Header + Data]          逐个转发                   dispatch 回调
// =============================================================================

#include "transport/unix_socket/unix_socket_backend.h"
#include "transport/common/broker_protocol.h"
#include "shm/shm_pool.h"

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <vector>

namespace dcms {

namespace {

// -----------------------------------------------------------------------------
// NotifyPayload: 模式A（socketpair）下通过 socket 传递的"通知包"
//
// 注意：只有 16 字节！不含任何消息数据。
// 接收方用 ptr 直接访问共享内存，这就是"零拷贝"的秘诀。
// -----------------------------------------------------------------------------
struct NotifyPayload {
    std::uintptr_t ptr; // 消息在共享内存中的虚拟地址（同进程直接可用）
    std::size_t    len; // 消息总长度（Header + 数据）
};

// 环境变量名
constexpr const char *kBrokerEnvEnable = "DCMS_USE_BROKER";   // 是否启用 broker 模式
constexpr const char *kBrokerEnvPath   = "DCMS_BROKER_SOCKET"; // broker socket 文件路径

// 获取 broker socket 文件路径（优先读环境变量，否则用默认路径）
std::string brokerSocketPath(const std::string &channel_name) {
    const char *from_env = std::getenv(kBrokerEnvPath);
    if (from_env && from_env[0] != '\0')
        return from_env;
    return "/tmp/" + channel_name + ".sock"; // 默认：/tmp/dcms_main.sock
}

// 检查是否应该使用 broker 模式（环境变量 DCMS_USE_BROKER 不为 "0"）
bool brokerEnabled() {
    const char *val = std::getenv(kBrokerEnvEnable);
    return val && std::strcmp(val, "0") != 0;
}

// 把一个 UNIX Datagram Socket 绑定到指定路径（先删除旧文件）
bool bindUnixDatagram(int fd, const std::string &path) {
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path.c_str());
    ::unlink(path.c_str()); // 先删掉旧文件，否则 bind 会失败
    return ::bind(fd, reinterpret_cast<const sockaddr *>(&addr), sizeof(addr)) == 0;
}

// 向指定 socket 文件路径发送数据
bool sendToPath(int fd, const std::string &path, const void *data, std::size_t len) {
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path.c_str());
    const ssize_t n =
        ::sendto(fd, data, len, MSG_NOSIGNAL,
                 reinterpret_cast<const sockaddr *>(&addr), sizeof(addr));
    return n == static_cast<ssize_t>(len);
}

} // namespace

UnixSocketBackend::UnixSocketBackend() = default;

UnixSocketBackend::~UnixSocketBackend() { shutdown(); }

// -----------------------------------------------------------------------------
// init: 根据环境变量决定使用哪种模式，并初始化对应的 socket
// -----------------------------------------------------------------------------
bool UnixSocketBackend::init(const std::string &channel_name) {
    broker_mode_ = brokerEnabled();

    if (broker_mode_) {
        // ── 模式 B：每个进程有一个专属的 Unix Datagram Socket 文件 ──
        // 路径格式：/tmp/dcms_client_{pid}_{this指针}.sock
        // 用 pid + this 指针保证路径唯一（同一机器多进程不冲突）
        broker_path_ = brokerSocketPath(channel_name);
        recv_fd_ = ::socket(AF_UNIX, SOCK_DGRAM, 0);
        if (recv_fd_ < 0)
            return false;
        send_fd_ = recv_fd_; // 同一个 fd 既收又发（Datagram socket）
        client_path_ = "/tmp/dcms_client_" + std::to_string(::getpid()) + "_" +
                       std::to_string(reinterpret_cast<std::uintptr_t>(this)) + ".sock";
        if (!bindUnixDatagram(recv_fd_, client_path_)) {
            shutdown();
            return false;
        }
        return true;
    }

    // ── 模式 A：创建 socketpair，两端连通 ──
    // fds[0] 用于发送，fds[1] 用于接收
    // 只有同进程内才能用（地址空间相同，指针才有意义）
    int fds[2];
    if (::socketpair(AF_UNIX, SOCK_DGRAM, 0, fds) != 0)
        return false;
    send_fd_ = fds[0];
    recv_fd_ = fds[1];
    return true;
}

// -----------------------------------------------------------------------------
// sendNotify: 发布方调用，发送"有消息了"的通知
//
// 模式A：只发 16 字节的 {ptr, len}（零拷贝！）
// 模式B：序列化完整消息，发给 broker 进程（有数据拷贝）
// -----------------------------------------------------------------------------
bool UnixSocketBackend::sendNotify(const void *shm_ptr, std::size_t len) {
    if (broker_mode_) {
        // broker 模式：把 [BrokerPacketHeader][完整消息] 打包发给 broker
        // BrokerPacketHeader 告诉 broker 这是 publish 操作，以及消息大小
        std::vector<std::uint8_t> packet(sizeof(BrokerPacketHeader) + len);
        auto *hdr = reinterpret_cast<BrokerPacketHeader *>(packet.data());
        hdr->command     = BrokerCommand::kPublish;
        hdr->topic       = 0; // broker 会从消息 header 里读真正的 topic
        hdr->payload_len = static_cast<std::uint32_t>(len);
        std::memset(hdr->client_path, 0, sizeof(hdr->client_path));
        // 把自己的 socket 路径也带上，broker 知道是谁发来的
        std::snprintf(hdr->client_path, sizeof(hdr->client_path), "%s",
                      client_path_.c_str());
        std::memcpy(packet.data() + sizeof(BrokerPacketHeader), shm_ptr, len);
        return sendToPath(send_fd_, broker_path_, packet.data(), packet.size());
    }

    // socketpair 模式：只发指针和长度，极小开销
    if (send_fd_ < 0)
        return false;
    const NotifyPayload payload{reinterpret_cast<std::uintptr_t>(shm_ptr), len};
    const ssize_t n = ::send(send_fd_, &payload, sizeof(payload), MSG_NOSIGNAL);
    return n == static_cast<ssize_t>(sizeof(payload));
}

// -----------------------------------------------------------------------------
// recvNotify: 接收方（RecvThread）调用，等待通知并取回消息指针
//
// 模式A：读出 {ptr, len}，ptr 直接就是 ShmPool 中的地址
// 模式B：读出完整消息字节流，从 ShmPool 申请内存，拷贝进去，返回指针
//        （broker 模式无法真正零拷贝，因为跨进程地址空间不同）
// -----------------------------------------------------------------------------
bool UnixSocketBackend::recvNotify(void *&shm_ptr, std::size_t &len, int timeout_ms) {
    if (recv_fd_ < 0)
        return false;

    // poll 等待数据到来，timeout_ms 超时后返回（让调用方有机会检查 running_ 标志）
    pollfd pfd{};
    pfd.fd     = recv_fd_;
    pfd.events = POLLIN;
    if (::poll(&pfd, 1, timeout_ms) <= 0)
        return false; // 超时或出错

    if (broker_mode_) {
        // broker 模式：收到完整消息，存入 ShmPool
        // 用临时栈缓冲区接收（最大 64KB），再复制到 ShmPool
        std::vector<std::uint8_t> tmp(64 * 1024);
        const ssize_t n = ::recv(recv_fd_, tmp.data(), tmp.size(), 0);
        if (n <= 0)
            return false;
        // 从 ShmPool 申请内存，这样 releaseRecvBuffer 能统一用 ShmPool::free 释放
        void *buf = ShmPool::instance().alloc(static_cast<std::size_t>(n));
        if (!buf)
            return false;
        std::memcpy(buf, tmp.data(), static_cast<std::size_t>(n));
        shm_ptr = buf;
        len     = static_cast<std::size_t>(n);
        return true;
    }

    // socketpair 模式：收到 {ptr, len}，ptr 就是发布方写入的共享内存地址
    NotifyPayload payload{};
    const ssize_t n = ::recv(recv_fd_, &payload, sizeof(payload), 0);
    if (n != static_cast<ssize_t>(sizeof(payload)))
        return false;
    shm_ptr = reinterpret_cast<void *>(payload.ptr); // 直接转换回指针
    len     = payload.len;
    return true;
}

// -----------------------------------------------------------------------------
// subscribeTopic / unsubscribeTopic:
//   socketpair 模式下不需要任何操作（同进程收所有 topic）
//   broker 模式下需要告知 broker "我要/不要这个 topic"
// -----------------------------------------------------------------------------
bool UnixSocketBackend::subscribeTopic(std::uint32_t topic) {
    if (!broker_mode_)
        return true; // socketpair 模式：直接返回成功，不需要注册
    return sendControl(static_cast<std::uint32_t>(BrokerCommand::kSubscribe), topic);
}

bool UnixSocketBackend::unsubscribeTopic(std::uint32_t topic) {
    if (!broker_mode_)
        return true;
    return sendControl(static_cast<std::uint32_t>(BrokerCommand::kUnsubscribe), topic);
}

bool UnixSocketBackend::usesSharedMemoryTransport() const {
    // socketpair 模式：接收方持有 SHM 指针，发布方不能提前释放
    // broker 模式：发布方发完数据就可以释放，接收方有自己的副本
    return !broker_mode_;
}

// -----------------------------------------------------------------------------
// releaseRecvBuffer: 释放 recvNotify 返回的缓冲区
//
// 两种模式下内存都通过 ShmPool 分配，统一用 ShmPool::free 释放
// （broker 模式在 recvNotify 中已改为从 ShmPool 分配）
// -----------------------------------------------------------------------------
void UnixSocketBackend::releaseRecvBuffer(void *ptr) {
    ShmPool::instance().free(ptr);
}

// 发送控制命令给 broker（subscribe/unsubscribe）
bool UnixSocketBackend::sendControl(std::uint32_t command, std::uint32_t topic) {
    BrokerPacketHeader hdr{};
    hdr.command     = static_cast<BrokerCommand>(command);
    hdr.topic       = topic;
    hdr.payload_len = 0;
    std::snprintf(hdr.client_path, sizeof(hdr.client_path), "%s", client_path_.c_str());
    return sendToPath(send_fd_, broker_path_, &hdr, sizeof(hdr));
}

void UnixSocketBackend::shutdown() {
    if (send_fd_ >= 0 && send_fd_ != recv_fd_) {
        ::close(send_fd_);
        send_fd_ = -1;
    }
    if (recv_fd_ >= 0) {
        ::close(recv_fd_);
        recv_fd_ = -1;
    }
    if (!client_path_.empty()) {
        ::unlink(client_path_.c_str()); // 删除 socket 文件
        client_path_.clear();
    }
    broker_path_.clear();
    broker_mode_ = false;
}

std::unique_ptr<IpcBackend> createUnixSocketBackend() {
    return std::make_unique<UnixSocketBackend>();
}

} // namespace dcms
