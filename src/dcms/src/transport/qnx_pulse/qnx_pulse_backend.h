#pragma once

#include "transport/common/ipc_backend.h"

namespace dcms {

class QnxPulseBackend final : public IpcBackend {
public:
    bool init(const std::string &channel_name) override;
    bool sendNotify(const void *shm_ptr, std::size_t len) override;
    bool recvNotify(void *&shm_ptr, std::size_t &len, int timeout_ms) override;
    void shutdown() override;
};

} // namespace dcms
