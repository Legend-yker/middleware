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

// 四阶段通信协议消息类型
// 阶段一：注册   进程 → Daemon
// 阶段二：发布声明 进程 → Daemon，Daemon 创建 shm_open 环形缓冲区
// 阶段三：订阅   进程 → Daemon，Daemon 通过 SCM_RIGHTS 把 eventfd 传给双方
// 阶段四：通信   Publisher 直写 shm + write(eventfd)，Daemon 不参与
enum class DaemonMsgType : std::uint32_t {
    kNodeRegister        = 1,
    kNodeRegisterAck     = 2,
    kAdvertise           = 3,
    kAdvertiseAck        = 4,
    kSubscribe           = 5,
    kSubscribeAck        = 6,  // eventfd 通过 SCM_RIGHTS 附带（不在结构体字段里）
    kNewSubscriberNotify = 7,  // Daemon → Publisher，通知有新订阅者，eventfd 通过 SCM_RIGHTS
    kError               = 255,
};

// 每条 PDU 的帧头，固定 8 字节，之后紧跟 payload_len 字节的具体消息体
struct DaemonMsgHeader {
    DaemonMsgType    type;
    std::uint32_t    payload_len;
};

// 阶段一：节点注册
struct NodeRegisterReq {
    char    node_name[kMaxNodeNameLen];
    pid_t   pid;
};

struct NodeRegisterAck {
    std::uint32_t  node_id;  // Daemon 分配的节点 ID，后续调试/日志用
    DcmsError      status;
};

// 阶段二：发布声明
// Daemon 收到后调用 shm_open + ftruncate 创建共享内存，自身不 mmap
struct AdvertiseReq {
    char           topic_name[kMaxTopicNameLen];
    std::uint32_t  slot_size;   // sizeof(T)，决定每个环形槽的大小
    std::uint32_t  ring_slots;  // 环形缓冲区槽位数
};

struct AdvertiseAck {
    char           shm_name[kMaxShmNameLen]; // 如 "/dcms_sensor_imu"，供 Publisher mmap
    std::uint64_t  shm_size;                 // 页对齐后的总字节数
    DcmsError      status;
};

// 阶段三：订阅
// SubscribeAck 的 eventfd 不在结构体字段里，通过 sendmsg SCM_RIGHTS 附带传递。
// SCM_RIGHTS 是 Unix 域套接字的特性：可以在进程间安全地传递文件描述符，
// 内核负责在接收进程中创建新的 fd 指向同一内核对象，无需共享内存或文件路径。
struct SubscribeReq {
    char topic_name[kMaxTopicNameLen];
};

struct SubscribeAck {
    char           shm_name[kMaxShmNameLen];
    std::uint64_t  shm_size;
    std::uint32_t  slot_size;
    std::uint32_t  ring_slots;
    DcmsError      status;
    // eventfd 通过 SCM_RIGHTS 附带，接收方用 recvmsg 取出
};

// kNewSubscriberNotify：无消息体，仅通过 SCM_RIGHTS 携带 eventfd 发给 Publisher

struct ErrorResp {
    DcmsError  code;
    char       message[64];
};

} // namespace dcms
