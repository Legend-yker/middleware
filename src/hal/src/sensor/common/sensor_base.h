#pragma once

#include "hal_export.h"
#include "sensor/common/sensor_types.h"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace dcms {
class Publisher;
enum class DataType : std::uint32_t;
} // namespace dcms

namespace hal {

class HAL_API SensorBase {
public:
    explicit SensorBase(SensorType type);
    virtual ~SensorBase();

    SensorBase(const SensorBase &) = delete;
    SensorBase &operator=(const SensorBase &) = delete;

    bool initFromConfigEntry(const nlohmann::json &item);

    virtual bool start();
    virtual void stop();

    int publish(const void *data, std::size_t len, dcms::DataType data_type);
    int publish(const void *data, std::size_t len, dcms::DataType data_type,
                std::uint64_t timestamp_capture);

    const std::string &id() const { return id_; }
    SensorType type() const { return type_; }
    bool enabled() const { return enabled_; }
    bool running() const { return running_; }
    std::uint32_t topicId() const { return topic_id_; }

protected:
    virtual bool doInitParams(const nlohmann::json &configEntry) = 0;

    std::string id_;
    SensorType type_{SensorType::kUnknown};
    bool enabled_{true};
    bool running_{false};
    std::uint32_t topic_id_{0};
    std::uint32_t publish_seq_{0};
    std::unique_ptr<dcms::Publisher> publisher_;
};

} // namespace hal
