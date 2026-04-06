#include "sensor/imu/imu_sensor.h"

namespace hal {

ImuSensor::ImuSensor() : SensorBase(SensorType::kImu) {}

bool ImuSensor::doInitParams(const nlohmann::json &configEntry) {
    driver_ = configEntry.value("driver", "stub");
    (void)configEntry.value("params", nlohmann::json::object());
    return true;
}

} // namespace hal
