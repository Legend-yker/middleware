#include "sensor/lidar/lidar_airy/airy_driver.h"

#include <iostream>

namespace hal {

AiryDriver::AiryDriver() = default;
AiryDriver::~AiryDriver() = default;

bool AiryDriver::doInitParams(const nlohmann::json &configEntry) {
    if (!LidarBase::doInitParams(configEntry))
        return false;

    const auto &params = configEntry.value("params", nlohmann::json::object());
    scan_mode_ = params.value("scan_mode", "single");
    rpm_ = params.value("rpm", 600u);

    // TODO: initialize sdk_core AirySDK
    return true;
}

bool AiryDriver::doStartCapture() {
    std::cout << "[Airy] start capture ip=" << ip_
              << " port=" << data_port_
              << " scan_mode=" << scan_mode_ << '\n';
    // TODO: call sdk_core start
    return true;
}

int AiryDriver::doStopCapture() {
    std::cout << "[Airy] stop capture\n";
    // TODO: call sdk_core stop
    return 0;
}

} // namespace hal
