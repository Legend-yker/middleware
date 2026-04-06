#include "transport/unix_socket/unix_socket_backend.h"

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <sys/socket.h>
#include <poll.h>
#include <unistd.h>

namespace dcms {

UnixSocketBackend::UnixSocketBackend() = default;

UnixSocketBackend::~UnixSocketBackend() { shutdown(); }

bool UnixSocketBackend::init(const std::string & /*channel_name*/) {
    int fds[2];
    if (::socketpair(AF_UNIX, SOCK_DGRAM, 0, fds) != 0)
        return false;
    send_fd_ = fds[0];
    recv_fd_ = fds[1];
    return true;
}

bool UnixSocketBackend::sendNotify(const void *shm_ptr, std::size_t len) {
    if (send_fd_ < 0)
        return false;

    struct NotifyPayload {
        std::uintptr_t ptr;
        std::size_t len;
    };
    NotifyPayload payload{reinterpret_cast<std::uintptr_t>(shm_ptr), len};

    ssize_t n = ::send(send_fd_, &payload, sizeof(payload), MSG_NOSIGNAL);
    return n == static_cast<ssize_t>(sizeof(payload));
}

bool UnixSocketBackend::recvNotify(void *&shm_ptr, std::size_t &len, int timeout_ms) {
    if (recv_fd_ < 0)
        return false;

    struct pollfd pfd{};
    pfd.fd = recv_fd_;
    pfd.events = POLLIN;
    int ret = ::poll(&pfd, 1, timeout_ms);
    if (ret <= 0)
        return false;

    struct NotifyPayload {
        std::uintptr_t ptr;
        std::size_t len;
    };
    NotifyPayload payload{};
    ssize_t n = ::recv(recv_fd_, &payload, sizeof(payload), 0);
    if (n != static_cast<ssize_t>(sizeof(payload)))
        return false;

    shm_ptr = reinterpret_cast<void *>(payload.ptr);
    len = payload.len;
    return true;
}

void UnixSocketBackend::shutdown() {
    if (send_fd_ >= 0) {
        ::close(send_fd_);
        send_fd_ = -1;
    }
    if (recv_fd_ >= 0) {
        ::close(recv_fd_);
        recv_fd_ = -1;
    }
}

std::unique_ptr<IpcBackend> createUnixSocketBackend() {
    return std::make_unique<UnixSocketBackend>();
}

} // namespace dcms
