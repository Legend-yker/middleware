#pragma once

#include "hal_export.h"
#include "sensor/common/sensor_base.h"

#include <cstdint>

namespace hal {

class HAL_API OdoSensor final : public SensorBase {
public:
    explicit OdoSensor();

    const std::string &interface() const { return interface_; }
    std::uint32_t ticksPerRev() const { return ticks_per_rev_; }
    double wheelRadius() const { return wheel_radius_; }

protected:
    bool doInitParams(const nlohmann::json &params) override;

private:
    std::string interface_; // "can", "spi", "gpio" etc.
    std::uint32_t ticks_per_rev_{1024};
    double wheel_radius_{0.3};
};

} // namespace hal
