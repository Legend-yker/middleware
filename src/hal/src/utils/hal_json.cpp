#include "utils/hal_json.h"

namespace hal {

bool HalJson::loadFromJson(const std::string &path, std::string &err) {
    if (!config_.loadFromJson(path, err))
        return false;

    version_ = config_.getString("version", "");
    mode_ = config_.getString("mode", "");
    project_ = config_.getString("project", "");

    if (!parseManagers(err))
        return false;
    if (!parseFunctions(err))
        return false;
    if (!parseSensors(err))
        return false;

    return true;
}

bool HalJson::parseManagers(std::string &err) {
    managers_.clear();
    for (const auto &item : config_.getArray("manager")) {
        if (!item.is_object())
            continue;
        ManagerItemConfig mc;
        mc.manager_item_ = item.value("manager_item", "");
        mc.params_ = item.value("params", nlohmann::json::object());
        managers_.push_back(std::move(mc));
    }
    return true;
}

bool HalJson::parseFunctions(std::string &err) {
    functions_.clear();
    for (const auto &item : config_.getArray("function")) {
        if (!item.is_object())
            continue;
        FunctionItemConfig fc;
        fc.func_item_ = item.value("func_item", "");
        fc.params_ = item.value("params", nlohmann::json::object());
        functions_.push_back(std::move(fc));
    }
    return true;
}

bool HalJson::parseSensors(std::string &err) {
    vision_sensors_.clear();
    lidar_sensors_.clear();
    imu_sensors_.clear();
    gnss_sensors_.clear();
    odo_sensors_.clear();

    for (const auto &group : config_.getArray("sensor")) {
        if (!group.is_object())
            continue;
        if (!parseSensorGroup(group, err))
            return false;
    }
    return true;
}

bool HalJson::parseSensorGroup(const nlohmann::json &group, std::string &err) {
    const std::string sensor_type = group.value("sensor_type", "");
    if (sensor_type.empty()) {
        err = "sensor group missing sensor_type";
        return false;
    }

    std::vector<SensorItemConfig> *target = nullptr;
    if (sensor_type == "vision")     target = &vision_sensors_;
    else if (sensor_type == "lidar") target = &lidar_sensors_;
    else if (sensor_type == "imu")   target = &imu_sensors_;
    else if (sensor_type == "gnss")  target = &gnss_sensors_;
    else if (sensor_type == "odo")   target = &odo_sensors_;
    else {
        err = "unknown sensor_type: " + sensor_type;
        return false;
    }

    const auto &items = group.value("sensor_item", nlohmann::json::array());
    for (const auto &item : items) {
        if (!item.is_object())
            continue;
        SensorItemConfig sc;
        sc.driver_name_ = item.value("driver_name", "");
        sc.common_config_ = item.value("driver_common_config", nlohmann::json::object());
        sc.driver_list_ = item.value("driver_list", nlohmann::json::array());
        target->push_back(std::move(sc));
    }
    return true;
}

} // namespace hal
