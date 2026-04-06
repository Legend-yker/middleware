#pragma once

#include "hal_export.h"
#include "sensor/common/sensor_base.h"

#include <cstdint>

namespace hal {

class HAL_API GnssSensor final : public SensorBase {
public:
    explicit GnssSensor();

    const std::string &protocol() const { return protocol_; }
    std::uint32_t baudRate() const { return baud_rate_; }
    const std::string &port() const { return port_; }

protected:
    bool doInitParams(const nlohmann::json &params) override;

private:
    std::string protocol_; // "nmea", "rtcm", "ublox" etc.
    std::string port_;     // serial port path, e.g. "/dev/ttyUSB0"
    std::uint32_t baud_rate_{115200};
};

} // namespace hal
