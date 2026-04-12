# DCMS - 数据通信管理服务

DCMS (Data Communication Management Service) 是一个**高性能跨进程发布订阅通信框架**，为机器人实时通信场景优化。

核心设计理念：**零拷贝 + 共享内存 + 事件通知** 混合架构。

---

## 目录

- [整体架构](#整体架构)
- [目录结构](#目录结构)
- [发布流程](#发布流程)
- [订阅与接收流程](#订阅与接收流程)
- [共享内存与零拷贝原理](#共享内存与零拷贝原理)
- [IPC 两种工作模式](#ipc-两种工作模式)
- [完整端到端数据流](#完整端到端数据流)
- [关键设计决策](#关键设计决策)
- [构建与测试](#构建与测试)

---

## 整体架构

DCMS 采用四层设计，从上到下依次是：

```
┌─────────────────────────────────────────────────────────┐
│                   第一层：C++ API                         │
│   Publisher<T> / Subscriber<T>                           │
│   提供类型安全的泛型封装，面向业务代码                      │
│   文件：src/api/publisher.h  src/api/subscriber.h        │
└──────────────────────┬──────────────────────────────────┘
                       │
┌──────────────────────▼──────────────────────────────────┐
│                   第二层：C API                            │
│   dcms_init / dcms_msg_publish / dcms_msg_subscribe      │
│   职责：序列号递增、时间戳填充、CRC32 计算、订阅表管理      │
│   文件：src/api/dcms_api.cpp                              │
└──────────────────────┬──────────────────────────────────┘
                       │
┌──────────────────────▼──────────────────────────────────┐
│                   第三层：传输层                           │
│   IpcBackend 接口 + 平台实现                              │
│   Linux  → UnixSocketBackend（socketpair / broker）      │
│   QNX    → QnxPulseBackend（MsgSendPulse / Receive）     │
│   文件：src/transport/                                    │
└──────────────────────┬──────────────────────────────────┘
                       │
┌──────────────────────▼──────────────────────────────────┐
│                   第四层：共享内存池                       │
│   ShmPool（单例）+ BuddyAllocator + SlabAllocator        │
│   当前为 malloc stub，后续替换为 shm_open + mmap          │
│   文件：src/shm/                                          │
└─────────────────────────────────────────────────────────┘
```

---

## 目录结构

```
src/dcms/
├── CMakeLists.txt
├── cmake/dcms_deps.cmake
├── config/dcms_example.json
├── src/
│   ├── dcms_export.h                        # 导出宏
│   ├── app/main.cpp                         # dcms_app 可执行（broker 进程）
│   ├── api/
│   │   ├── dcms.h / dcms.cpp                # C++ 入口：dcms::init / shutdown
│   │   ├── dcms_api.h / dcms_api.cpp        # C API 核心实现
│   │   ├── publisher.h / publisher.cpp      # Publisher<T> 封装
│   │   └── subscriber.h                     # Subscriber<T> 封装
│   ├── common/
│   │   ├── dcms_types.h / .cpp              # 错误码、DcmsMsgHeader、msgDataPtr
│   │   ├── dcms_msg.h                       # MsgMeta、Msg<T>、nowUs()
│   │   └── crc32.h / .cpp                   # CRC32 查表实现
│   ├── manager/
│   │   └── recv_thread.h / .cpp             # 后台接收线程 + 订阅表分发
│   ├── shm/
│   │   ├── shm_pool.h / .cpp                # 共享内存池（单例）
│   │   ├── buddy_allocator.h / .cpp         # Buddy 分配器（stub）
│   │   └── slab_allocator.h / .cpp          # Slab 分配器（stub）
│   ├── transport/
│   │   ├── common/
│   │   │   ├── ipc_backend.h / .cpp         # IpcBackend 抽象接口
│   │   │   └── broker_protocol.h            # broker 协议包结构
│   │   ├── unix_socket/
│   │   │   └── unix_socket_backend.h / .cpp # Linux IPC 实现
│   │   └── qnx_pulse/
│   │       └── qnx_pulse_backend.h / .cpp   # QNX IPC 实现（stub）
│   └── utils/
│       └── msg_queue.h / .cpp               # 无锁 SPSC 环形队列
└── third_party/
```

---

## 发布流程

用户调用 `pub.publish(msg)` 后，消息经过以下步骤传递：

```
Publisher::publish(msg)
    │
    │ ① CRC 校验：如果 meta.payload_crc32 不为 0，验证数据完整性
    │   失败 → 返回 kDcmsErrCrcMismatch，阻止脏数据入队
    ▼
    把 [MsgMeta][payload] 打包成连续缓冲区
    （小于 4KB 用栈，否则用 vector，避免堆分配）
    │
    ▼
dcms_msg_publish(topic, buf, total_len)
    │
    │ ② 向 ShmPool 申请一块内存
    │   内存布局：[DcmsMsgHeader 56字节][用户数据 N字节]
    ▼
    ┌─────────────────────────────────┐
    │  DcmsMsgHeader                  │  ← hdr 指针
    │    topic_id    = 0x1a2b3c4d     │
    │    seq         = 42（自动递增）  │
    │    msg_type    = kPublish        │
    │    data_offset = 56             │  ← 数据从第 56 字节开始
    │    data_len    = N              │
    │    timestamp_* = 当前微秒时间   │
    │    crc32       = 0              │  ← 先清零
    ├─────────────────────────────────┤
    │  [你的数据 N 字节]               │  ← msgDataPtr(hdr) 指这里
    └─────────────────────────────────┘
    │
    │ ③ memcpy 用户数据填入 header 后面
    │ ④ 对整块内存（header + data）计算 CRC32，写入 hdr->crc32
    │   （计算时 crc32 字段为 0，接收方用同样方式验证）
    ▼
IpcBackend::sendNotify(buf_ptr, total_len)
    │
    ├── socketpair 模式（默认，同进程）：
    │     只发 16 字节：{ ptr=0x7f3abc00, len=128 }
    │     接收方用这个指针直接访问内存 → 零拷贝！
    │
    └── broker 模式（跨进程）：
          序列化 [BrokerPacketHeader][完整消息] 发给 broker 进程
          broker 再转发给订阅了该 topic 的所有客户端
    │
    ▼
    socketpair 模式：buf 由接收方释放（接收方还在用）
    broker   模式：数据已拷贝发出，立即释放 buf
```

---

## 订阅与接收流程

### 注册阶段（用户调用时）

```
Subscriber<T> sub("lidar/front", my_callback)
    │
    │ ① hash("lidar/front") → topic_id（uint32_t）
    │   不同字符串可能碰撞，生产环境建议用枚举 TopicId
    ▼
dcms_msg_subscribe(topic_id, &staticCb, this)
    │
    │ ② 如果这是该 topic 的第一个订阅者：
    │   ipc->subscribeTopic(topic_id)
    │   broker 模式：告知 broker"请把这个 topic 的消息发给我"
    │   socketpair 模式：直接返回 true（接收所有 topic）
    │
    │ ③ 在 RecvThread 的订阅表里登记：
    │   subs_[topic_id].push_back({ cb_key, user_ptr, lambda })
    │
    │ ④ topic 引用计数 +1
    ▼
    注册完毕，后续由后台线程自动驱动
```

### 接收阶段（后台线程持续运行）

```
RecvThread::loop()  ← 独立后台线程，dcms::init() 时启动
    │
    ├─[循环]─────────────────────────────────────────────┐
    │                                                     │
    │  backend_->recvNotify(ptr, len, timeout=50ms)       │
    │      │                                             │
    │      ├── 超时（50ms 无消息）→ 检查 running_ → ─────┘
    │      │   （保证 stop() 能及时退出，不会卡死）
    │      │
    │      └── 收到消息 → ptr 指向 [Header][Data]
    │
    │  ① 基本校验：ptr != null && len >= sizeof(Header)
    │
    │  ② CRC32 校验：
    │       saved = hdr->crc32
    │       hdr->crc32 = 0        ← 必须清零再算
    │       actual = crc32(整块内存)
    │       hdr->crc32 = saved    ← 还原
    │       actual != saved → 丢弃，releaseRecvBuffer，continue
    │
    │  ③ dispatch(hdr)
    │       ┌─ 持锁 sub_mutex_
    │       │  找到 subs_[topic_id]
    │       │  复制订阅列表到 local_subs
    │       └─ 释放锁 sub_mutex_   ← 锁只持有复制阶段，不持有回调阶段
    │
    │       for each entry in local_subs:
    │           entry.cb(hdr, data_ptr, data_len)
    │               │
    │               ▼
    │           Subscriber<T>::cCallback()
    │               memcpy → T 对象
    │               调用用户 lambda: my_callback(meta, data)
    │
    │  ④ releaseRecvBuffer(ptr) → ShmPool::free(ptr)
    │
    └─[继续循环]
```

---

## 共享内存与零拷贝原理

### 当前实现（malloc stub）

```
同进程内，socketpair 模式：

  发布方                         共享内存（ShmPool，当前 = malloc）
  ─────                         ─────────────────────────────────
  ShmPool::alloc() → buf
  写入 Header + Data              ┌──────────────────┐
  buf 地址 = 0x7f3abc00    ─────→│ [Header][Data]   │
                                  └──────────────────┘
  sendNotify: 发送 16 字节              ↑
  { ptr=0x7f3abc00, len=128 }          │  同进程，地址相同
                                        │
  接收方（RecvThread）                  │
  recvNotify: 收到 ptr=0x7f3abc00 ─────┘
  直接访问，无任何数据拷贝！
  这就是"零拷贝"
```

### 未来真实 SHM 实现（多进程）

```
进程 A（发布方）              物理内存              进程 B（订阅方）
────────────                 ────────────          ─────────────
shm_open("dcms_shm")                              shm_open("dcms_shm")
mmap() → 0x7f00_0000  ─────→ [同一块物理页] ←──── mmap() → 0x7f80_0000
         （虚拟地址A）                              （虚拟地址B，不同！）

进程A 写入数据到 0x7f00_1000（offset=0x1000）
IPC 只发送 offset=0x1000（4字节整数）
进程B 收到 offset，用 自己的基址 + offset 访问数据
→ 完全没有跨进程数据拷贝
→ 大块数据（如激光雷达帧）性能优势极大
```

### 为什么 broker 模式不能零拷贝？

```
broker 模式涉及跨进程，且没有共享内存映射：
  进程A → [完整数据] → broker进程 → [完整数据] → 进程B
  每次转发都有一次 memcpy，共 2 次拷贝。

broker 模式的优势是：
  - 支持任意进程间通信，不需要提前共享内存
  - 支持一对多广播（一次发布，多个订阅者各收一份）
  - 开发调试简单（只需设置环境变量）
```

---

## IPC 两种工作模式

| 特性 | socketpair 模式（默认） | broker 模式 |
|------|----------------------|------------|
| 启用方式 | 默认 | `DCMS_USE_BROKER=1` |
| 适用场景 | 同一进程内 | 不同进程间 |
| 数据传输 | 零拷贝（传指针） | 2 次 memcpy |
| 是否需要 broker 进程 | 否 | 是（需先启动 dcms_app） |
| 延迟 | 极低（~微秒） | 较低（~10 微秒） |
| broker socket 路径 | 无 | `/tmp/dcms_main.sock`（可用 `DCMS_BROKER_SOCKET` 覆盖） |

### socketpair 模式数据流

```
发布方进程
  ShmPool::alloc(buf)
  填写 Header + Data
  send(sock_w, {ptr, len}, 8字节)
       │
       │ Unix socketpair
       ▼
RecvThread（同进程）
  recv(sock_r) → 收到 {ptr, len}
  ptr 直接是 ShmPool 地址，访问无需拷贝
  dispatch → 回调
  ShmPool::free(ptr)
```

### broker 模式数据流

```
发布方进程                      broker 进程 (dcms_app)          订阅方进程
──────────                      ──────────────────────          ──────────
sendto(broker_sock,         →   recv: [BrokerHeader][Data]
  [BrokerHeader][Data])         查 subscribers[topic]      →    recvfrom(my_sock)
                                for each client:                ShmPool::alloc(buf)
                                  sendto(client_sock, data)     memcpy(buf, data)
                                                                dispatch → 回调
                                                                ShmPool::free(buf)
```

---

## 完整端到端数据流

```
你的代码                C++ API            C API              IPC              内存池
────────                ───────            ─────              ───              ────

pub.publish(msg)
    │
    │ hash topic
    │ CRC check
    ▼
Publisher::publish()
    │ 打包 meta+data
    ▼
                    dcms_msg_publish()
                        │
                        │ alloc ──────────────────────────────────────→ ShmPool
                        │ fill header                                        │
                        │ memcpy data                                   返回 buf ptr
                        │ calc CRC32                                         │
                        │ sendNotify(ptr) ──────────────→ IpcBackend         │
                        │                                  send {ptr,len}    │
                                                              │（8字节）      │
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━│━━━━━━━━━━━━━━━│━
RecvThread::loop()（后台线程）                                │               │
                                                              ▼               │
                                                    recvNotify() 收到 {ptr}   │
                                                              │               │
                                                    ptr ──────────────────────┘
                                                    （直接访问，零拷贝）
                                                              │
                                                    CRC32 校验
                                                              │
                                                    dispatch(hdr)
                                                              │
                    Subscriber::cCallback() ←────────────────┘
                        │
                        │ memcpy → T 对象
                        ▼
你的回调(meta, data) ←──┘
```

---

## 关键设计决策

### 1. dispatch 为什么先复制订阅列表再调用回调？

```
错误做法（持锁调回调）：
  RecvThread                    用户回调内
  持有 sub_mutex_           →   调用 subscribe()
                                 → 尝试加锁 sub_mutex_ → 死锁！

正确做法（复制后释放锁）：
  RecvThread
  持有 sub_mutex_
  复制 local_subs = subs_[topic]
  释放 sub_mutex_           →   无锁调用回调
                                 → subscribe() 可以正常加锁
```

### 2. CRC32 为什么要先清零再计算？

```
发布方：
  hdr->crc32 = 0
  hdr->crc32 = crc32(整块内存)   ← 计算时 crc32 字段值为 0

接收方：
  saved = hdr->crc32
  hdr->crc32 = 0                 ← 还原到计算时的状态
  actual = crc32(整块内存)
  if actual == saved → 完整

如果不清零，crc32 字段本身的值会影响计算结果，形成循环依赖。
```

### 3. 序列号（seq）的作用

```
seq 是一个全局自增计数器（atomic<uint32_t>）：
  - 去重：同一消息被多路径收到时可检测
  - 丢包检测：接收方发现 seq 跳跃，说明中间有包丢失
  - 与 topic_id 组合使用效果更好
```

### 4. releaseRecvBuffer 为什么单独抽成虚函数？

```
不同传输模式的内存来源不同：
  socketpair 模式：ptr 指向 ShmPool 内存 → 用 ShmPool::free()
  broker 模式：ptr 是 recvNotify 内部从 ShmPool 申请的 → 也用 ShmPool::free()

但当 BuddyAllocator 真正实现后，ShmPool::free 必须知道 ptr 来自哪个 pool，
releaseRecvBuffer 虚函数让每个 backend 自己负责，不会写错。
```

---

## 构建与测试

### 构建

```bash
# Release 构建（推荐）
bash release_how_to/build_x86

# Debug 构建（含 examples）
BUILD_TYPE=Debug bash release_how_to/build_x86

# 产物路径
release_how_to/build-x86/bin/tests/   # 测试可执行文件
release_how_to/build-x86/bin/dcms/    # dcms_app（broker 进程）
release_how_to/build-x86/lib/         # libdcms.a
```

### 运行测试

```bash
# 基础 smoke 测试（socketpair 模式，同进程）
./release_how_to/build-x86/bin/tests/dcms_smoke

# broker 模式测试（需要先启动 broker 进程）
# 终端 1：启动 broker
./release_how_to/build-x86/bin/dcms/dcms_app

# 终端 2：用 broker 模式跑 smoke 测试
DCMS_USE_BROKER=1 ./release_how_to/build-x86/bin/tests/dcms_smoke
```

### smoke 测试覆盖项

| 测试项 | 验证内容 |
|--------|---------|
| CRC32 一致性 | 相同数据两次计算结果相同且不为 0 |
| MsgQueue 边界 | capacity=4 时第 5 次 push 返回 false |
| MsgMeta 布局 | `sizeof(MsgMeta) == 40`（ABI 稳定性保证） |
| 发布→订阅 | 发布后 500ms 内订阅方收到消息 |
| Metadata 传递 | module_id / seq / timestamp_capture 完整到达 |
| CRC 防篡改 | 故意传错 payload_crc32 → 返回 `kDcmsErrCrcMismatch` |
| shutdown 后发布 | 返回 `kDcmsErrNotInit` |

### 用 ctest 运行（可选）

```bash
ctest --test-dir release_how_to/build-x86 --output-on-failure -R dcms
```

---

## 当前已知限制

| 限制 | 说明 | 计划 |
|------|------|------|
| ShmPool 用 malloc 模拟 | 尚未接入真正的 `shm_open + mmap` | 后续迭代 |
| BuddyAllocator 是 stub | 直接调用 malloc/free | 后续实现真正的伙伴算法 |
| QnxPulseBackend 是 stub | 所有接口返回 false | QNX 平台适配时填充 |
| socketpair 模式仅限同进程 | 跨进程需要 broker 模式 | 真 SHM 实现后可支持跨进程零拷贝 |
| topic_id 用字符串 hash | 可能碰撞 | 生产环境建议改为枚举常量 |
