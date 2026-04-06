#pragma once

#include "hal_export.h"
#include "sensor/common/sensor_base.h"
#include "utils/hal_json.h"

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace hal {

class SensorFactory;

// Holds all sensor **instance objects** organized by type.
// Converts JSON text config → real C++ sensor objects.
class HAL_API Config {
public:
    Config() = default;

    bool init(const HalJson &hal_json, std::string &err);
    void clear();

    const std::vector<std::unique_ptr<SensorBase>> &cameras() const { return cameras_; }
    const std::vector<std::unique_ptr<SensorBase>> &lidars() const { return lidars_; }
    const std::vector<std::unique_ptr<SensorBase>> &imus() const { return imus_; }
    const std::vector<std::unique_ptr<SensorBase>> &gnss() const { return gnss_; }
    const std::vector<std::unique_ptr<SensorBase>> &odos() const { return odos_; }

    SensorBase *get(const std::string &id);
    const SensorBase *get(const std::string &id) const;

    std::size_t size() const;

    void forEach(const std::function<void(SensorBase &)> &fn);
    void forEach(const std::function<void(const SensorBase &)> &fn) const;

private:
    bool createFromItems(const std::vector<HalJson::SensorItemConfig> &items,
                         std::vector<std::unique_ptr<SensorBase>> &out,
                         std::string &err);
    bool addToIndex(SensorBase *s, std::string &err);

    std::vector<std::unique_ptr<SensorBase>> cameras_;
    std::vector<std::unique_ptr<SensorBase>> lidars_;
    std::vector<std::unique_ptr<SensorBase>> imus_;
    std::vector<std::unique_ptr<SensorBase>> gnss_;
    std::vector<std::unique_ptr<SensorBase>> odos_;

    std::unordered_map<std::string, SensorBase *> id_index_;
};

} // namespace hal
