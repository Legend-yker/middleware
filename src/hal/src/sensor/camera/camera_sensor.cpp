#include "sensor/camera/camera_sensor.h"

namespace hal {

CameraSensor::CameraSensor() : SensorBase(SensorType::kCamera) {}

bool CameraSensor::doInitParams(const nlohmann::json &configEntry) {
    driver_ = configEntry.value("driver", "stub");
    (void)configEntry.value("params", nlohmann::json::object());
    return true;
}

} // namespace hal
