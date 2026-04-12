#include "transport/common/ipc_backend.h"
#include "shm/shm_pool.h"

namespace dcms {

IpcBackend::~IpcBackend() = default;

void IpcBackend::releaseRecvBuffer(void *ptr) {
    ShmPool::instance().free(ptr);
}

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
