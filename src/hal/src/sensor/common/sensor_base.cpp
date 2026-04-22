#include "sensor/common/sensor_base.h"
#include "api/dcms.h"
#include "dlog/dlog.h"
#include "sensor/common/sensor_types.h"

namespace hal {

SensorBase::SensorBase(SensorType type) : type_(type) {}

SensorBase::~SensorBase() { stop(); }

bool SensorBase::initFromConfigEntry(const nlohmann::json &item) {
    if (!item.is_object())
        return false;

    if (item.contains("id") && item["id"].is_string())
        id_ = item["id"].get<std::string>();
    else if (item.contains("id") && item["id"].is_number())
        id_ = std::to_string(item["id"].get<int>());
    else if (item.contains("sensor_name"))
        id_ = item["sensor_name"].get<std::string>();

    topic_id_ = static_cast<std::uint32_t>(std::hash<std::string>{}(id_));
    enabled_  = item.value("enabled", true);

    const std::uint32_t sz = payloadSize();
    if (sz > 0 && dcms::defaultNode().valid())
        publisher_ = dcms::defaultNode().advertise_raw(id_, sz);

    return doInitParams(item);
}

bool SensorBase::start() {
    if (!enabled_)
        return true;
    running_ = true;
    return true;
}

int SensorBase::stop() {
    running_ = false;
    return 0;
}

int SensorBase::publish(const void *data, std::size_t len) {
    if (!publisher_.valid())
        return -1;
    return publisher_.publish(data, static_cast<std::uint32_t>(len));
}

static std::string makeSlogPrefix(hal::SensorType t, const std::string &id) {
    return std::string("[MOD:hal][") + sensorTypeToString(t) + ":" + id + "]";
}

int SensorBase::slogDebug(const std::string &msg) const {
    dlog::log(dlog::Level::kDebug, makeSlogPrefix(type_, id_), msg);
    return 0;
}
int SensorBase::slogInfo(const std::string &msg) const {
    dlog::log(dlog::Level::kInfo,  makeSlogPrefix(type_, id_), msg);
    return 0;
}
int SensorBase::slogWarn(const std::string &msg) const {
    dlog::log(dlog::Level::kWarn,  makeSlogPrefix(type_, id_), msg);
    return 0;
}
int SensorBase::slogError(const std::string &msg) const {
    dlog::log(dlog::Level::kError, makeSlogPrefix(type_, id_), msg);
    return 0;
}

} // namespace hal
