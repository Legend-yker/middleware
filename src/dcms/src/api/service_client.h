#pragma once

#include "common/dcms_types.h"

#include <cstring>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace dcms {

// ServiceClient<Req, Resp>
//
// Connects to /tmp/dcms_svc_<service_name>.sock for each call().
// Sends [uint32_t len][Req], receives [uint32_t len][Resp].
// Req and Resp must be trivially copyable POD types.
//
// Usage (via NodeHandle):
//   auto cli = nh.serviceClient<AddReq, AddResp>("add");
//   AddResp resp{};
//   int rc = cli.call(req, resp);

template <typename Req, typename Resp>
class ServiceClient {
public:
    ServiceClient() = default;

    explicit ServiceClient(const std::string &service_name)
        : socket_path_("/tmp/dcms_svc_" + sanitize(service_name) + ".sock") {}

    bool valid() const { return !socket_path_.empty(); }

    // Returns kDcmsOk on success.
    // Returns kDcmsErrDaemonNotFound if server is not running.
    // Returns kDcmsErrNotImpl on protocol error or timeout.
    int call(const Req &req, Resp &resp, int timeout_ms = 1000) {
        int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd < 0) return kDcmsErrNotImpl;

        struct timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, socket_path_.c_str(),
                     sizeof(addr.sun_path) - 1);

        if (::connect(fd, reinterpret_cast<sockaddr *>(&addr),
                      sizeof(addr)) < 0) {
            ::close(fd);
            return kDcmsErrDaemonNotFound;
        }

        std::uint32_t req_len = static_cast<std::uint32_t>(sizeof(Req));
        if (!sendExact(fd, &req_len, sizeof(req_len)) ||
            !sendExact(fd, &req, sizeof(Req))) {
            ::close(fd);
            return kDcmsErrNotImpl;
        }

        std::uint32_t resp_len = 0;
        if (!recvExact(fd, &resp_len, sizeof(resp_len)) ||
            resp_len != sizeof(Resp) ||
            !recvExact(fd, &resp, sizeof(Resp))) {
            ::close(fd);
            return kDcmsErrNotImpl;
        }

        ::close(fd);
        return kDcmsOk;
    }

private:
    static std::string sanitize(const std::string &name) {
        std::string out = name;
        for (char &c : out)
            if (c == '/' || c == ' ' || c == ':') c = '_';
        return out;
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

    std::string socket_path_;
};

} // namespace dcms
