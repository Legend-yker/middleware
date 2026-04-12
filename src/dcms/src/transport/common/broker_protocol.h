#pragma once

#include "common/dcms_types.h"

#include <cstddef>
#include <cstdint>

namespace dcms {

enum class BrokerCommand : std::uint32_t {
    kSubscribe = 1,
    kUnsubscribe = 2,
    kPublish = 3,
};

constexpr std::size_t kBrokerClientPathSize = 108;

struct BrokerPacketHeader {
    BrokerCommand command;
    TopicId topic;
    std::uint32_t payload_len;
    char client_path[kBrokerClientPathSize];
};

} // namespace dcms
