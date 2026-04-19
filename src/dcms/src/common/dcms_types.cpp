#include "common/dcms_types.h"

namespace dcms {

const char *dcmsErrorToString(int err) {
    switch (err) {
    case kDcmsOk:
        return "ok";
    case kDcmsErrNotInit:
        return "not initialized";
    case kDcmsErrBusy:
        return "busy (queue full)";
    case kDcmsErrShmFail:
        return "shared memory failure";
    case kDcmsErrNotImpl:
        return "not implemented";
    case kDcmsErrInvalidArg:
        return "invalid argument";
    case kDcmsErrCrcMismatch:
        return "CRC mismatch";
    case kDcmsErrTimeout:
        return "timeout";
    case kDcmsErrNoSubscriber:
        return "no subscriber";
    case kDcmsErrDaemonNotFound:
        return "daemon not found";
    case kDcmsErrTopicExists:
        return "topic already exists";
    default:
        return "unknown error";
    }
}

} // namespace dcms
