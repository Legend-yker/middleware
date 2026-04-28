#pragma once

#include "sensor/lidar/common/lidar_base.h"
#include "sensor/lidar/common/lidar_frame_manager.h"
#include "utils/hal_thread.h"

#include <atomic>
#include <cstdint>
#include <memory>

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
    void udpRecvLoop();

    std::uint16_t gps_port_{10110};
    bool          dual_return_{false};
    std::uint32_t rpm_{600};         // 转速，决定帧率和每帧包数
    int           sock_fd_{-1};

    std::atomic<bool>                  running_{false};
    hal::utils::SingleThread           recv_thread_;
    std::unique_ptr<LidarFrameManager> frame_mgr_;
    std::uint32_t                      frame_seq_{0};
};

} // namespace hal
