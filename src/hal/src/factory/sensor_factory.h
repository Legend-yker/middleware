#pragma once

#include "hal_export.h"
#include "sensor/common/sensor_base.h"

#include <nlohmann/json.hpp>

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

namespace hal {

class HAL_API SensorFactory {
public:
    using Creator = std::function<std::unique_ptr<SensorBase>(const nlohmann::json &fullItem)>;

    static SensorFactory &instance();

    SensorFactory(const SensorFactory &) = delete;
    SensorFactory &operator=(const SensorFactory &) = delete;

    void registerType(const std::string &type, Creator creator);

    // Old path: type + driver combined key
    std::unique_ptr<SensorBase> createFromConfigEntry(const nlohmann::json &item, std::string &err);

    // New path: lookup by driver_name directly (e.g. "LIDAR_PANDAR128")
    std::unique_ptr<SensorBase> createByDriver(const std::string &driver_name,
                                                const nlohmann::json &instance_config,
                                                std::string &err);

private:
    SensorFactory() = default;

    std::unordered_map<std::string, Creator> creators_;
};

HAL_API void registerBuiltinSensors();

} // namespace hal
