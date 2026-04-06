#pragma once

#include "transport/common/ipc_backend.h"

#include <cstdint>
#include <string>

namespace dcms {

class UnixSocketBackend final : public IpcBackend {
public:
    UnixSocketBackend();
    ~UnixSocketBackend() override;

    bool init(const std::string &channel_name) override;
    bool sendNotify(const void *shm_ptr, std::size_t len) override;
    bool recvNotify(void *&shm_ptr, std::size_t &len, int timeout_ms) override;
    bool subscribeTopic(std::uint32_t topic) override;
    bool unsubscribeTopic(std::uint32_t topic) override;
    bool usesSharedMemoryTransport() const override;
    void shutdown() override;

private:
    bool sendControl(std::uint32_t command, std::uint32_t topic);

    int send_fd_{-1};
    int recv_fd_{-1};
    bool broker_mode_{false};
    std::string broker_path_;
    std::string client_path_;
};

} // namespace dcms
