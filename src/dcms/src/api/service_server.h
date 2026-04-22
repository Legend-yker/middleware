#pragma once

#include "common/dcms_types.h"

#include <atomic>
#include <cstring>
#include <functional>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>

namespace dcms {

// RPC 服务端，监听 /tmp/dcms_svc_<service_name>.sock。
// 每次连接：接收 [uint32_t len][Req]，调用 handler，回复 [uint32_t len][Resp]。
// Req 和 Resp 必须是 trivially copyable 的 POD 类型。
// 不经过 Daemon，Publisher/Subscriber 也不涉及，是独立的点对点通信。
// 用法：auto srv = nh.advertiseService<AddReq, AddResp>("add",
//           [](const AddReq &req, AddResp &resp){ resp.sum = req.a + req.b; });

template <typename Req, typename Resp>
class ServiceServer {
public:
    using Handler = std::function<void(const Req &, Resp &)>;

    ServiceServer() = default;

    ServiceServer(const std::string &service_name, Handler handler)
        : socket_path_("/tmp/dcms_svc_" + sanitize(service_name) + ".sock"),
          handler_(std::move(handler)) {
        listen_fd_ = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (listen_fd_ < 0) return;

        ::unlink(socket_path_.c_str());

        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, socket_path_.c_str(),
                     sizeof(addr.sun_path) - 1);

        if (::bind(listen_fd_, reinterpret_cast<sockaddr *>(&addr),
                   sizeof(addr)) < 0 ||
            ::listen(listen_fd_, 8) < 0) {
            ::close(listen_fd_);
            listen_fd_ = -1;
            return;
        }

        running_ = true;
        accept_thread_ = std::thread([this] { acceptLoop(); });
    }

    ~ServiceServer() {
        running_ = false;
        if (listen_fd_ >= 0) {
            ::shutdown(listen_fd_, SHUT_RDWR);
            ::close(listen_fd_);
            listen_fd_ = -1;
        }
        if (accept_thread_.joinable())
            accept_thread_.join();
        if (!socket_path_.empty())
            ::unlink(socket_path_.c_str());
    }

    ServiceServer(ServiceServer &&o) noexcept
        : socket_path_(std::move(o.socket_path_)),
          listen_fd_(o.listen_fd_),
          handler_(std::move(o.handler_)),
          running_(o.running_.load()),
          accept_thread_(std::move(o.accept_thread_)) {
        o.listen_fd_ = -1;
        o.running_   = false;
    }

    ServiceServer(const ServiceServer &)            = delete;
    ServiceServer &operator=(const ServiceServer &) = delete;
    ServiceServer &operator=(ServiceServer &&)      = delete;

    bool valid() const { return listen_fd_ >= 0; }

private:
    static std::string sanitize(const std::string &name) {
        std::string out = name;
        for (char &c : out)
            if (c == '/' || c == ' ' || c == ':') c = '_';
        return out;
    }

    void acceptLoop() {
        while (running_.load(std::memory_order_relaxed)) {
            pollfd pfd{listen_fd_, POLLIN, 0};
            if (::poll(&pfd, 1, 100) <= 0) continue; // 100ms 超时，方便 running_=false 后退出
            if (!(pfd.revents & POLLIN)) continue;
            int client_fd = ::accept4(listen_fd_, nullptr, nullptr, SOCK_CLOEXEC);
            if (client_fd < 0) continue;
            handleClient(client_fd); // 同步处理：读 Req → 调 handler → 发 Resp
            ::close(client_fd);      // 单次 RPC 完成后关闭连接
        }
    }

    void handleClient(int fd) {
        // 协议格式：[uint32_t len][Req 结构体]
        std::uint32_t req_len = 0;
        if (!recvExact(fd, &req_len, sizeof(req_len))) return;
        if (req_len != sizeof(Req)) return; // 类型不匹配，拒绝

        Req req{};
        if (!recvExact(fd, &req, sizeof(Req))) return;

        Resp resp{};
        handler_(req, resp); // 调用用户注册的处理函数

        // 回复格式：[uint32_t len][Resp 结构体]
        std::uint32_t resp_len = static_cast<std::uint32_t>(sizeof(Resp));
        if (!sendExact(fd, &resp_len, sizeof(resp_len))) return;
        sendExact(fd, &resp, sizeof(Resp));
    }

    bool recvExact(int fd, void *buf, std::size_t n) {
        std::size_t got = 0;
        while (got < n) {
            ssize_t r = ::recv(fd, static_cast<char *>(buf) + got, n - got, 0);
            if (r <= 0) return false;
            got += static_cast<std::size_t>(r);
        }
        return true;
    }

    bool sendExact(int fd, const void *buf, std::size_t n) {
        std::size_t sent = 0;
        while (sent < n) {
            ssize_t r = ::send(fd, static_cast<const char *>(buf) + sent,
                               n - sent, MSG_NOSIGNAL);
            if (r <= 0) return false;
            sent += static_cast<std::size_t>(r);
        }
        return true;
    }

    std::string       socket_path_;
    int               listen_fd_{-1};
    Handler           handler_;
    std::atomic<bool> running_{false};
    std::thread       accept_thread_;
};

} // namespace dcms
