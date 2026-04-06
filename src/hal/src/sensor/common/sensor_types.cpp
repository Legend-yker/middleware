#include "sensor/common/sensor_types.h"

namespace hal {

const char *sensorTypeToString(SensorType t) {
    switch (t) {
    case SensorType::kCamera:  return "camera";
    case SensorType::kImu:     return "imu";
    case SensorType::kLidar:   return "lidar";
    case SensorType::kGnss:    return "gnss";
    case SensorType::kOdo:     return "odo";
    default:                   return "unknown";
    }
}

SensorType sensorTypeFromString(const std::string &s) {
    if (s == "camera") return SensorType::kCamera;
    if (s == "imu")    return SensorType::kImu;
    if (s == "lidar")  return SensorType::kLidar;
    if (s == "gnss")   return SensorType::kGnss;
    if (s == "odo")    return SensorType::kOdo;
    return SensorType::kUnknown;
}

} // namespace hal
