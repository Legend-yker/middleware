#pragma once

#include "sensor/lidar/common/lidar_base.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>

namespace hal {

class HAL_API Pandar128Driver final : public LidarBase {
public:
    Pandar128Driver();
    ~Pandar128Driver() override;

protected:
    bool doInitParams(const nlohmann::json &configEntry) override;
    bool doStartCapture() override;
    int  doStopCapture() override;

private:
    bool openSocket();
    int  closeSocket();
    int  udpRecvLoop();

    std::uint16_t gps_port_{10110};
    bool dual_return_{false};
    int socket_fd_{-1};

    std::atomic<bool> capturing_{false};
    std::unique_ptr<std::thread> recv_thread_;
    std::uint32_t frame_seq_{0};
};

} // namespace hal
