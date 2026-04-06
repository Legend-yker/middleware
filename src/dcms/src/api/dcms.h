#pragma once

#include "dcms_export.h"

#include <string>

namespace dcms {

DCMS_API int init(const std::string &config_path = "");

DCMS_API void shutdown();

} // namespace dcms
