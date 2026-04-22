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

// 每条消息的元数据头，随 payload 一起写入共享内存
struct MsgMeta {
    std::uint32_t module_id;          // 产生此消息的模块（ModuleId 枚举值）
    std::uint32_t instance_id;        // 传感器/组件的 topic 哈希，区分同类型多实例
    std::uint32_t data_type;          // payload 类型标签（DataType 枚举值）
    std::uint32_t seq;                // 应用层消息序号，用于检测丢帧
    std::uint64_t timestamp_capture;  // 数据实际采集时刻（微秒，steady_clock）
    std::uint64_t timestamp_publish;  // publish() 被调用时刻（微秒），延迟 = publish - capture
    std::uint32_t payload_len;        // 紧跟在本结构体之后的 payload 字节数
    std::uint32_t payload_crc32;      // 仅对 payload 字节计算的 CRC32，用于完整性校验
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
