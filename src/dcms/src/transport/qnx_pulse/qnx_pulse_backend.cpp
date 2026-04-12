#include "transport/qnx_pulse/qnx_pulse_backend.h"
#include "common/dcms_types.h"

#include <memory>

namespace dcms {

bool QnxPulseBackend::init(const std::string & /*channel_name*/) { return false; }
bool QnxPulseBackend::sendNotify(const void *, std::size_t) { return false; }
bool QnxPulseBackend::recvNotify(void *&, std::size_t &, int) { return false; }
bool QnxPulseBackend::subscribeTopic(std::uint32_t) { return false; }
bool QnxPulseBackend::unsubscribeTopic(std::uint32_t) { return false; }
bool QnxPulseBackend::usesSharedMemoryTransport() const { return true; }
void QnxPulseBackend::shutdown() {}

std::unique_ptr<IpcBackend> createQnxPulseBackend() {
    return std::make_unique<QnxPulseBackend>();
}

} // namespace dcms
