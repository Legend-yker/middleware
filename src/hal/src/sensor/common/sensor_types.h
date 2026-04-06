#pragma once

#include "hal_export.h"

#include <string>

namespace hal {

enum class SensorType { kUnknown, kCamera, kImu, kLidar, kGnss, kOdo };

HAL_API const char *sensorTypeToString(SensorType t);
HAL_API SensorType sensorTypeFromString(const std::string &s);

} // namespace hal
