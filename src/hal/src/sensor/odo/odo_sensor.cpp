#include "sensor/odo/odo_sensor.h"

namespace hal {

OdoSensor::OdoSensor() : SensorBase(SensorType::kOdo) {}

bool OdoSensor::doInitParams(const nlohmann::json &configEntry) {
    const auto &params = configEntry.value("params", nlohmann::json::object());
    interface_ = params.value("interface", "can");
    ticks_per_rev_ = params.value("ticks_per_rev", 1024u);
    wheel_radius_ = params.value("wheel_radius", 0.3);
    return true;
}

} // namespace hal
