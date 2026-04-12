#include "sensor/common/sensor_base.h"
#include "api/publisher.h"
#include "common/dcms_msg.h"
#include "common/crc32.h"
#include "dlog/dlog.h"
#include "sensor/common/sensor_types.h"

#include <functional>

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
    publisher_ = std::make_unique<dcms::Publisher>(topic_id_);

    enabled_ = item.value("enabled", true);
    return doInitParams(item);
}

bool SensorBase::start() {
    if (!enabled_)
        return true;
    running_ = true;
    return true;
}

void SensorBase::stop() { running_ = false; }

int SensorBase::publish(const void *data, std::size_t len,
                        dcms::DataType data_type) {
    return publish(data, len, data_type, dcms::nowUs());
}

int SensorBase::publish(const void *data, std::size_t len,
                        dcms::DataType data_type,
                        std::uint64_t timestamp_capture) {
    if (!publisher_)
        return -1;

    dcms::MsgMeta meta{};
    meta.module_id = static_cast<std::uint32_t>(dcms::ModuleId::kHal);
    meta.instance_id = topic_id_;
    meta.data_type = static_cast<std::uint32_t>(data_type);
    meta.seq = publish_seq_++;
    meta.timestamp_capture = timestamp_capture;
    meta.timestamp_publish = dcms::nowUs();
    meta.payload_len = static_cast<std::uint32_t>(len);
    meta.payload_crc32 = dcms::crc32(data, len);

    return publisher_->publish(meta, data, static_cast<std::uint32_t>(len));
}

// ---------------------------------------------------------------------------
// 传感器日志辅助实现
//
// 前缀格式：[MOD:hal][lidar:57]
//   - sensorTypeToString(type_) → "lidar" / "camera" / "imu" / ...
//   - id_  → JSON 里读到的传感器 ID（如 "57" 或 "front_left"）
// ---------------------------------------------------------------------------
static std::string makeSlogPrefix(hal::SensorType t, const std::string &id) {
    return std::string("[MOD:hal][") + sensorTypeToString(t) + ":" + id + "]";
}

void SensorBase::slogDebug(const std::string &msg) const {
    dlog::log(dlog::Level::kDebug, makeSlogPrefix(type_, id_), msg);
}
void SensorBase::slogInfo(const std::string &msg) const {
    dlog::log(dlog::Level::kInfo,  makeSlogPrefix(type_, id_), msg);
}
void SensorBase::slogWarn(const std::string &msg) const {
    dlog::log(dlog::Level::kWarn,  makeSlogPrefix(type_, id_), msg);
}
void SensorBase::slogError(const std::string &msg) const {
    dlog::log(dlog::Level::kError, makeSlogPrefix(type_, id_), msg);
}

} // namespace hal
