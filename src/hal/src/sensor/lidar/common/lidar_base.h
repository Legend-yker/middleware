#pragma once

#include "hal_export.h"
#include "sensor/common/sensor_base.h"
#include "sensor/lidar/common/lidar_frame.h"

#include <cstdint>
#include <string>

namespace hal {

class HAL_API LidarBase : public SensorBase {
public:
    explicit LidarBase();
    ~LidarBase() override;

    std::uint32_t payloadSize() const override { return sizeof(LidarFrame); }

    const std::string &model() const { return model_; }
    const std::string &transport() const { return transport_; }
    const std::string &ip() const { return ip_; }
    std::uint16_t dataPort() const { return data_port_; }

    bool start() override;
    int  stop() override;

protected:
    bool doInitParams(const nlohmann::json &configEntry) override;

    virtual bool doStartCapture() = 0;
    virtual int  doStopCapture()  = 0;

    std::string model_;
    std::string transport_;
    std::string ip_;
    std::uint16_t data_port_{0};
};

} // namespace hal
