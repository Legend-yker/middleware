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
    void doStopCapture() override;

private:
    // 1) UDP 虽然是无连接协议，但接收端仍然需要 socket + bind 才能监听端口。
    // 2) 这里拆成 open/close 两个函数，是为了让启动、停止、异常处理的资源释放更清晰。
    bool openSocket();
    void closeSocket();

    // 3) 后台接收线程主循环：
    //    fake UDP 包 -> 解析成 LidarFrame -> 发布到 DCMS
    void udpRecvLoop();

    std::uint16_t gps_port_{10110};
    bool dual_return_{false};
    int socket_fd_{-1};

    std::atomic<bool> capturing_{false};
    std::unique_ptr<std::thread> recv_thread_;
    std::uint32_t frame_seq_{0};
};

} // namespace hal
