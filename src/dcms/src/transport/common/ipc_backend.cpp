#include "transport/common/ipc_backend.h"

namespace dcms {

IpcBackend::~IpcBackend() = default;

std::unique_ptr<IpcBackend> createUnixSocketBackend();
std::unique_ptr<IpcBackend> createQnxPulseBackend();

std::unique_ptr<IpcBackend> createIpcBackend() {
#if defined(DCMS_IPC_QNX_PULSE)
    return createQnxPulseBackend();
#else
    return createUnixSocketBackend();
#endif
}

} // namespace dcms
