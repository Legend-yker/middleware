#pragma once

#include "dcms_export.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace dcms {

class DCMS_API IpcBackend {
public:
    virtual ~IpcBackend();

    virtual bool init(const std::string &channel_name) = 0;
    virtual bool sendNotify(const void *shm_ptr, std::size_t len) = 0;
    virtual bool recvNotify(void *&shm_ptr, std::size_t &len, int timeout_ms) = 0;
    virtual bool subscribeTopic(std::uint32_t topic) = 0;
    virtual bool unsubscribeTopic(std::uint32_t topic) = 0;
    virtual bool usesSharedMemoryTransport() const = 0;
    virtual void shutdown() = 0;
};

DCMS_API std::unique_ptr<IpcBackend> createIpcBackend();

} // namespace dcms
