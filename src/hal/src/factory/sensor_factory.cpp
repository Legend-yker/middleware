#include "factory/sensor_factory.h"

#include "sensor/camera/camera_sensor.h"
#include "sensor/gnss/gnss_sensor.h"
#include "sensor/imu/imu_sensor.h"
#include "sensor/lidar/lidar_airy/airy_driver.h"
#include "sensor/lidar/lidar_pandar128/pandar128_driver.h"
#include "sensor/odo/odo_sensor.h"

namespace hal {

SensorFactory &SensorFactory::instance() {
    static SensorFactory inst;
    return inst;
}

void SensorFactory::registerType(const std::string &type, Creator creator) {
    creators_[type] = std::move(creator);
}

std::unique_ptr<SensorBase> SensorFactory::createFromConfigEntry(const nlohmann::json &item,
                                                                 std::string &err) {
    std::string type = item.value("type", "");
    const std::string driver = item.value("driver", "");

    if (type == "lidar" && !driver.empty())
        type = "lidar." + driver;

    const auto it = creators_.find(type);
    if (it == creators_.end()) {
        err = "unknown sensor type/driver: " + type;
        return nullptr;
    }
    auto s = it->second(item);
    if (!s) {
        err = "factory creator returned null for type: " + type;
        return nullptr;
    }
    if (!s->initFromConfigEntry(item)) {
        err = "sensor init failed, type=" + type + " id=" + item.value("id", "");
        return nullptr;
    }
    return s;
}

std::unique_ptr<SensorBase> SensorFactory::createByDriver(const std::string &driver_name,
                                                           const nlohmann::json &instance_config,
                                                           std::string &err) {
    const auto it = creators_.find(driver_name);
    if (it == creators_.end()) {
        err = "unknown driver_name: " + driver_name;
        return nullptr;
    }
    auto s = it->second(instance_config);
    if (!s) {
        err = "factory creator returned null for driver: " + driver_name;
        return nullptr;
    }
    if (!s->initFromConfigEntry(instance_config)) {
        err = "sensor init failed, driver=" + driver_name;
        return nullptr;
    }
    return s;
}

void registerBuiltinSensors() {
    SensorFactory &f = SensorFactory::instance();

    // New driver_name keys (hal_online.json format)
    f.registerType("CAMERA_NVIDIA",   [](const nlohmann::json &) { return std::make_unique<CameraSensor>(); });
    f.registerType("LIDAR_PANDAR128", [](const nlohmann::json &) { return std::make_unique<Pandar128Driver>(); });
    f.registerType("LIDAR_AIRY",      [](const nlohmann::json &) { return std::make_unique<AiryDriver>(); });

    // Legacy keys (sensors_example.json backward compat)
    f.registerType("camera",          [](const nlohmann::json &) { return std::make_unique<CameraSensor>(); });
    f.registerType("imu",             [](const nlohmann::json &) { return std::make_unique<ImuSensor>(); });
    f.registerType("gnss",            [](const nlohmann::json &) { return std::make_unique<GnssSensor>(); });
    f.registerType("odo",             [](const nlohmann::json &) { return std::make_unique<OdoSensor>(); });
    f.registerType("lidar.pandar128", [](const nlohmann::json &) { return std::make_unique<Pandar128Driver>(); });
    f.registerType("lidar.airy",      [](const nlohmann::json &) { return std::make_unique<AiryDriver>(); });
}

namespace {
struct BuiltinSensorRegistrar {
    BuiltinSensorRegistrar() { registerBuiltinSensors(); }
};
const BuiltinSensorRegistrar g_builtin_sensor_registrar;
} // namespace

} // namespace hal
