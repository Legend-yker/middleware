#include "transport/qnx_pulse/qnx_pulse_backend.h"
#include "common/dcms_types.h"

namespace dcms {

bool QnxPulseBackend::init(const std::string & /*channel_name*/) { return false; }
bool QnxPulseBackend::sendNotify(const void *, std::size_t) { return false; }
bool QnxPulseBackend::recvNotify(void *&, std::size_t &, int) { return false; }
void QnxPulseBackend::shutdown() {}

std::unique_ptr<IpcBackend> createQnxPulseBackend() {
    return std::make_unique<QnxPulseBackend>();
}

} // namespace dcms
