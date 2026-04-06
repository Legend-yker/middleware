#pragma once

#include "hal_export.h"
#include "sensor/common/sensor_base.h"

namespace hal {

class HAL_API CameraSensor final : public SensorBase {
public:
    explicit CameraSensor();

    const std::string &driver() const { return driver_; }

protected:
    bool doInitParams(const nlohmann::json &params) override;

private:
    std::string driver_;
};

} // namespace hal
