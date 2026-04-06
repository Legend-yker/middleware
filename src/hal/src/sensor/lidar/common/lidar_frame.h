#pragma once

#include <cstdint>

namespace hal {

struct LidarPoint {
    float x;
    float y;
    float z;
    std::uint8_t intensity;
    std::uint8_t ring;
    std::uint8_t padding[2];
};

static_assert(sizeof(LidarPoint) == 16, "LidarPoint must be 16 bytes");

struct LidarFrame {
    std::uint32_t frame_id;
    std::uint32_t num_points;
    std::uint64_t timestamp_us;
    static constexpr std::uint32_t kMaxDemoPoints = 16;
    LidarPoint points[kMaxDemoPoints];
};

} // namespace hal
