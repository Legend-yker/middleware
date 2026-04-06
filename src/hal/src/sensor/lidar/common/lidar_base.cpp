#include "sensor/lidar/common/lidar_base.h"

namespace hal {

LidarBase::LidarBase() : SensorBase(SensorType::kLidar) {}

LidarBase::~LidarBase() = default;

bool LidarBase::doInitParams(const nlohmann::json &configEntry) {
    const auto &params = configEntry.value("params", nlohmann::json::object());
    model_ = params.value("model", "");
    transport_ = params.value("transport", "udp");

    if (params.contains("target_ip"))
        ip_ = params.value("target_ip", "0.0.0.0");
    else
        ip_ = params.value("ip", "0.0.0.0");

    const int p = params.value("port", 0);
    if (p < 0 || p > 65535)
        return false;
    data_port_ = static_cast<std::uint16_t>(p);
    return true;
}

bool LidarBase::start() {
    if (!SensorBase::start())
        return false;
    if (!enabled())
        return true;
    return doStartCapture();
}

void LidarBase::stop() {
    if (running())
        doStopCapture();
    SensorBase::stop();
}

} // namespace hal
