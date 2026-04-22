#pragma once

#include "hal_export.h"
#include "sensor/common/sensor_types.h"
#include "api/publisher.h"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <string>

namespace hal {

class HAL_API SensorBase {
public:
    explicit SensorBase(SensorType type);
    virtual ~SensorBase();

    SensorBase(const SensorBase &) = delete;
    SensorBase &operator=(const SensorBase &) = delete;

    bool initFromConfigEntry(const nlohmann::json &item);

    virtual bool start();
    virtual int  stop();

    // Publish raw payload into the ring buffer.
    int publish(const void *data, std::size_t len);

    const std::string &id() const { return id_; }

    int slogDebug(const std::string &msg) const;
    int slogInfo (const std::string &msg) const;
    int slogWarn (const std::string &msg) const;
    int slogError(const std::string &msg) const;

    SensorType    type()    const { return type_; }
    bool          enabled() const { return enabled_; }
    bool          running() const { return running_; }
    std::uint32_t topicId() const { return topic_id_; }

    // Returns the ring-buffer slot size for this sensor's frame type.
    // Subclasses must override if they want to publish.
    virtual std::uint32_t payloadSize() const { return 0; }

protected:
    virtual bool doInitParams(const nlohmann::json &configEntry) = 0;

    std::string   id_;
    SensorType    type_{SensorType::kUnknown};
    bool          enabled_{true};
    bool          running_{false};
    std::uint32_t topic_id_{0};
    dcms::Publisher publisher_;
};

} // namespace hal
