#include "transport/common/broker_protocol.h"
#include "common/dcms_types.h"

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {
std::atomic<bool> g_running{true};
void onSignal(int) { g_running = false; }

std::string brokerSocketPath() {
    const char *from_env = std::getenv("DCMS_BROKER_SOCKET");
    if (from_env && from_env[0] != '\0')
        return from_env;
    return "/tmp/dcms_main.sock";
}

bool bindUnixDatagram(int fd, const std::string &path) {
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path.c_str());
    ::unlink(path.c_str());
    return ::bind(fd, reinterpret_cast<const sockaddr *>(&addr), sizeof(addr)) == 0;
}

bool sendToClient(int fd, const std::string &path, const void *data, std::size_t len) {
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path.c_str());
    const ssize_t n =
        ::sendto(fd, data, len, MSG_NOSIGNAL, reinterpret_cast<const sockaddr *>(&addr),
                 sizeof(addr));
    return n == static_cast<ssize_t>(len);
}

} // namespace

int main() {
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    const std::string server_path = brokerSocketPath();
    const int fd = ::socket(AF_UNIX, SOCK_DGRAM, 0);
    if (fd < 0) {
        std::cerr << "[dcms_app] socket create failed\n";
        return EXIT_FAILURE;
    }
    if (!bindUnixDatagram(fd, server_path)) {
        std::cerr << "[dcms_app] bind failed path=" << server_path << '\n';
        ::close(fd);
        return EXIT_FAILURE;
    }

    std::unordered_map<dcms::TopicId, std::unordered_set<std::string>> subscribers;
    std::vector<std::uint8_t> packet(64 * 1024);

    std::cout << "[dcms_app] broker listening at " << server_path << '\n';
    while (g_running.load()) {
        pollfd pfd{};
        pfd.fd = fd;
        pfd.events = POLLIN;
        const int poll_rc = ::poll(&pfd, 1, 200);
        if (poll_rc <= 0)
            continue;

        const ssize_t n = ::recv(fd, packet.data(), packet.size(), 0);
        if (n < static_cast<ssize_t>(sizeof(dcms::BrokerPacketHeader)))
            continue;

        const auto *hdr =
            reinterpret_cast<const dcms::BrokerPacketHeader *>(packet.data());
        const std::string client_path(hdr->client_path);

        if (hdr->command == dcms::BrokerCommand::kSubscribe) {
            subscribers[hdr->topic].insert(client_path);
            std::cout << "[dcms_app] subscribe topic=0x" << std::hex << hdr->topic
                      << std::dec << " client=" << client_path << '\n';
            continue;
        }

        if (hdr->command == dcms::BrokerCommand::kUnsubscribe) {
            auto it = subscribers.find(hdr->topic);
            if (it != subscribers.end()) {
                it->second.erase(client_path);
                if (it->second.empty())
                    subscribers.erase(it);
            }
            std::cout << "[dcms_app] unsubscribe topic=0x" << std::hex << hdr->topic
                      << std::dec << " client=" << client_path << '\n';
            continue;
        }

        if (hdr->command != dcms::BrokerCommand::kPublish)
            continue;

        if (static_cast<std::size_t>(n) !=
            sizeof(dcms::BrokerPacketHeader) + hdr->payload_len) {
            continue;
        }

        const void *payload = packet.data() + sizeof(dcms::BrokerPacketHeader);
        if (hdr->payload_len < sizeof(dcms::DcmsMsgHeader))
            continue;

        const auto *msg = static_cast<const dcms::DcmsMsgHeader *>(payload);
        auto it = subscribers.find(msg->topic_id);
        if (it == subscribers.end())
            continue;

        for (auto sub_it = it->second.begin(); sub_it != it->second.end();) {
            if (!sendToClient(fd, *sub_it, payload, hdr->payload_len)) {
                sub_it = it->second.erase(sub_it);
            } else {
                ++sub_it;
            }
        }

        std::cout << "[dcms_app] publish topic=0x" << std::hex << msg->topic_id
                  << std::dec << " bytes=" << hdr->payload_len
                  << " subs=" << it->second.size() << '\n';
    }

    ::close(fd);
    ::unlink(server_path.c_str());
    std::cout << "[dcms_app] stopped\n";
    return EXIT_SUCCESS;
}
