#pragma once

#include "common/dcms_types.h"

#include <cstdint>
#include <sys/types.h>

namespace dcms {

constexpr const char    *kDaemonSocketPath = "/tmp/dcms_daemon.sock";
constexpr std::uint32_t  kDefaultRingSlots = 8;
constexpr std::size_t    kMaxTopicNameLen  = 64;
constexpr std::size_t    kMaxNodeNameLen   = 64;
constexpr std::size_t    kMaxShmNameLen    = 64;

enum class DaemonMsgType : std::uint32_t {
    kNodeRegister        = 1,
    kNodeRegisterAck     = 2,
    kAdvertise           = 3,
    kAdvertiseAck        = 4,
    kSubscribe           = 5,
    kSubscribeAck        = 6,  // eventfd via SCM_RIGHTS
    kNewSubscriberNotify = 7,  // Daemon → Publisher, eventfd via SCM_RIGHTS
    kError               = 255,
};

// Framing header prepended to every PDU
struct DaemonMsgHeader {
    DaemonMsgType    type;
    std::uint32_t    payload_len;
};

// Phase 1: Node registration
struct NodeRegisterReq {
    char    node_name[kMaxNodeNameLen];
    pid_t   pid;
};

struct NodeRegisterAck {
    std::uint32_t  node_id;
    DcmsError      status;
};

// Phase 2: Advertise
struct AdvertiseReq {
    char           topic_name[kMaxTopicNameLen];
    std::uint32_t  slot_size;   // sizeof(T)
    std::uint32_t  ring_slots;
};

struct AdvertiseAck {
    char           shm_name[kMaxShmNameLen];
    std::uint64_t  shm_size;
    DcmsError      status;
};

// Phase 3: Subscribe
// SubscribeAck carries eventfd via SCM_RIGHTS ancillary data (not in struct)
struct SubscribeReq {
    char topic_name[kMaxTopicNameLen];
};

struct SubscribeAck {
    char           shm_name[kMaxShmNameLen];
    std::uint64_t  shm_size;
    std::uint32_t  slot_size;
    std::uint32_t  ring_slots;
    DcmsError      status;
};

// kNewSubscriberNotify: no payload body; eventfd via SCM_RIGHTS only

struct ErrorResp {
    DcmsError  code;
    char       message[64];
};

} // namespace dcms
