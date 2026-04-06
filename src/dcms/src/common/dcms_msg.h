#pragma once

#include "dcms_export.h"

#include <chrono>
#include <cstddef>
#include <cstdint>

namespace dcms {

enum class ModuleId : std::uint32_t {
    kUnknown = 0,
    kHal = 1,
    kPhm = 2,
    kDcms = 3,
    kSec = 4,
    kOta = 5,
    kFms = 6,
};

enum class DataType : std::uint32_t {
    kRaw = 0,
    kLidarFrame = 1,
    kCameraFrame = 2,
    kImuData = 3,
    kGnssData = 4,
    kOdoData = 5,
};

struct MsgMeta {
    std::uint32_t module_id;          // ModuleId: which module produced this
    std::uint32_t instance_id;        // topic hash of the sensor/component
    std::uint32_t data_type;          // DataType tag for the payload
    std::uint32_t seq;                // application-level sequence number
    std::uint64_t timestamp_capture;  // when the data was actually captured (us)
    std::uint64_t timestamp_publish;  // when publish() was called (us)
    std::uint32_t payload_len;        // byte size of the payload following this struct
    std::uint32_t payload_crc32;      // CRC32 of just the payload bytes
};

static_assert(sizeof(MsgMeta) == 40, "MsgMeta layout unexpected");

template <typename T>
struct Msg {
    MsgMeta meta;
    T data;
};

inline std::uint64_t nowUs() {
    using namespace std::chrono;
    return static_cast<std::uint64_t>(
        duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count());
}

} // namespace dcms
