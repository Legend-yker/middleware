#pragma once

#include "sensor/lidar/common/lidar_base.h"

namespace hal {

class HAL_API AiryDriver final : public LidarBase {
public:
    AiryDriver();
    ~AiryDriver() override;

protected:
    bool doInitParams(const nlohmann::json &configEntry) override;
    bool doStartCapture() override;
    int  doStopCapture() override;

private:
    std::string scan_mode_; // "single", "dual"
    std::uint32_t rpm_{600};
};

} // namespace hal
