#include "manager/sensor_manager.h"

namespace hal {

SensorManager &SensorManager::instance() {
    static SensorManager inst;
    return inst;
}

bool SensorManager::loadFromJson(const std::string &path, std::string &err) {
    if (!hal_json_.loadFromJson(path, err))
        return false;

    if (!config_.init(hal_json_, err))
        return false;

    return true;
}

void SensorManager::clear() {
    stopAll();
    config_.clear();
}

bool SensorManager::startAll(std::string &err) {
    bool ok = true;
    config_.forEach([&](SensorBase &s) {
        if (!ok || !s.enabled())
            return;
        if (!s.start()) {
            err = "start failed for sensor id=" + s.id();
            ok = false;
        }
    });
    if (ok)
        err.clear();
    return ok;
}

void SensorManager::stopAll() {
    config_.forEach([](SensorBase &s) { s.stop(); });
}

} // namespace hal
