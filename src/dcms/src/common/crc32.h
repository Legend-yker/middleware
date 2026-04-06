#pragma once

#include "dcms_export.h"

#include <cstddef>
#include <cstdint>

namespace dcms {

DCMS_API std::uint32_t crc32(const void *data, std::size_t len);

DCMS_API std::uint32_t crc32Update(std::uint32_t prev, const void *data, std::size_t len);

} // namespace dcms
