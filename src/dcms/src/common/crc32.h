#pragma once

#include "dcms_export.h"

#include <cstddef>
#include <cstdint>

namespace dcms {

// 计算 data[0..len) 的 CRC32 校验值
DCMS_API std::uint32_t crc32(const void *data, std::size_t len);

// 增量计算：在已有校验值 prev 的基础上继续追加 data[0..len)
// 用于分片数据的校验：先算头部，再用 crc32Update 追加 payload
DCMS_API std::uint32_t crc32Update(std::uint32_t prev, const void *data, std::size_t len);

} // namespace dcms
