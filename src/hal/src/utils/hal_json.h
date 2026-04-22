#pragma once

#include "utils/config.h"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace hal {

class HalJson {
public:
    struct SensorItemConfig {
        std::string driver_name_;
        nlohmann::json common_config_;
        nlohmann::json driver_list_;
    };

    struct ManagerItemConfig {
        std::string manager_item_;
        nlohmann::json params_;
    };

    struct FunctionItemConfig {
        std::string func_item_;
        nlohmann::json params_;
    };

    HalJson() = default;

    bool loadFromJson(const std::string &path, std::string &err);

    const std::string &version() const { return version_; }
    const std::string &mode() const { return mode_; }
    const std::string &project() const { return project_; }

    const std::vector<ManagerItemConfig> &managers() const { return managers_; }
    const std::vector<FunctionItemConfig> &functions() const { return functions_; }

    const std::vector<SensorItemConfig> &visionSensors() const { return vision_sensors_; }
    const std::vector<SensorItemConfig> &lidarSensors() const { return lidar_sensors_; }
    const std::vector<SensorItemConfig> &imuSensors() const { return imu_sensors_; }
    const std::vector<SensorItemConfig> &gnssSensors() const { return gnss_sensors_; }
    const std::vector<SensorItemConfig> &odoSensors() const { return odo_sensors_; }

    const hal::utils::Config &rawConfig() const { return config_; }

private:
    bool parseManagers(std::string &err);
    bool parseFunctions(std::string &err);
    bool parseSensors(std::string &err);
    bool parseSensorGroup(const nlohmann::json &group, std::string &err);

    hal::utils::Config config_;
    std::string version_;
    std::string mode_;
    std::string project_;

    std::vector<ManagerItemConfig> managers_;
    std::vector<FunctionItemConfig> functions_;

    std::vector<SensorItemConfig> vision_sensors_;
    std::vector<SensorItemConfig> lidar_sensors_;
    std::vector<SensorItemConfig> imu_sensors_;
    std::vector<SensorItemConfig> gnss_sensors_;
    std::vector<SensorItemConfig> odo_sensors_;
};

} // namespace hal
