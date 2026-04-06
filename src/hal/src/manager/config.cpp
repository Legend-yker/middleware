#include "manager/config.h"

#include "factory/sensor_factory.h"

namespace hal {

bool Config::init(const HalJson &hal_json, std::string &err) {
    clear();

    if (!createFromItems(hal_json.visionSensors(), cameras_, err)) return false;
    if (!createFromItems(hal_json.lidarSensors(),  lidars_,  err)) return false;
    if (!createFromItems(hal_json.imuSensors(),    imus_,    err)) return false;
    if (!createFromItems(hal_json.gnssSensors(),   gnss_,    err)) return false;
    if (!createFromItems(hal_json.odoSensors(),    odos_,    err)) return false;

    return true;
}

void Config::clear() {
    id_index_.clear();
    cameras_.clear();
    lidars_.clear();
    imus_.clear();
    gnss_.clear();
    odos_.clear();
}

bool Config::createFromItems(const std::vector<HalJson::SensorItemConfig> &items,
                             std::vector<std::unique_ptr<SensorBase>> &out,
                             std::string &err) {
    for (const auto &item : items) {
        for (const auto &inst : item.driver_list_) {
            nlohmann::json entry;
            if (inst.is_object())
                entry = inst;
            else if (inst.is_number())
                entry["id"] = inst;
            else
                continue;

            if (!item.common_config_.empty())
                entry["driver_common_config"] = item.common_config_;

            std::string sub_err;
            auto s = SensorFactory::instance().createByDriver(
                item.driver_name_, entry, sub_err);
            if (!s) {
                err = sub_err;
                return false;
            }
            if (!addToIndex(s.get(), err))
                return false;
            out.push_back(std::move(s));
        }
    }
    return true;
}

bool Config::addToIndex(SensorBase *s, std::string &err) {
    const std::string &sid = s->id();
    if (sid.empty()) {
        err = "sensor id missing in config entry";
        return false;
    }
    if (id_index_.count(sid) != 0) {
        err = "duplicate sensor id: " + sid;
        return false;
    }
    id_index_[sid] = s;
    return true;
}

SensorBase *Config::get(const std::string &id) {
    const auto it = id_index_.find(id);
    return it == id_index_.end() ? nullptr : it->second;
}

const SensorBase *Config::get(const std::string &id) const {
    const auto it = id_index_.find(id);
    return it == id_index_.end() ? nullptr : it->second;
}

std::size_t Config::size() const {
    return cameras_.size() + lidars_.size() + imus_.size() +
           gnss_.size() + odos_.size();
}

void Config::forEach(const std::function<void(SensorBase &)> &fn) {
    auto apply = [&](std::vector<std::unique_ptr<SensorBase>> &v) {
        for (auto &s : v) fn(*s);
    };
    apply(cameras_);
    apply(lidars_);
    apply(imus_);
    apply(gnss_);
    apply(odos_);
}

void Config::forEach(const std::function<void(const SensorBase &)> &fn) const {
    auto apply = [&](const std::vector<std::unique_ptr<SensorBase>> &v) {
        for (const auto &s : v) fn(*s);
    };
    apply(cameras_);
    apply(lidars_);
    apply(imus_);
    apply(gnss_);
    apply(odos_);
}

} // namespace hal
