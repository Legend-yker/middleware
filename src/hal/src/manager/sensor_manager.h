#pragma once

#include "hal_export.h"
#include "manager/config.h"
#include "utils/hal_json.h"

#include <string>

namespace hal {

class HAL_API SensorManager {
public:
    static SensorManager &instance();

    SensorManager(const SensorManager &) = delete;
    SensorManager &operator=(const SensorManager &) = delete;

    bool loadFromJson(const std::string &path, std::string &err);
    void clear();

    bool startAll(std::string &err);
    void stopAll();

    Config &config() { return config_; }
    const Config &config() const { return config_; }

    const HalJson &halJson() const { return hal_json_; }

private:
    SensorManager() = default;
    ~SensorManager() = default;

    HalJson hal_json_;
    Config config_;
};

} // namespace hal
