#include "sensor/gnss/gnss_sensor.h"

namespace hal {

GnssSensor::GnssSensor() : SensorBase(SensorType::kGnss) {}

bool GnssSensor::doInitParams(const nlohmann::json &configEntry) {
    const auto &params = configEntry.value("params", nlohmann::json::object());
    protocol_ = params.value("protocol", "nmea");
    port_ = params.value("port", "/dev/ttyUSB0");
    baud_rate_ = params.value("baud_rate", 115200u);
    return true;
}

} // namespace hal
