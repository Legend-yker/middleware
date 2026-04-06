# DCMS - 数据通信管理服务

## 模块职责

DCMS (Data Communication Management Service) 是一个**高性能跨进程发布订阅通信框架**，为机器人实时通信场景优化。

核心设计理念：**零拷贝 + 共享内存 + 事件通知** 混合架构。

## 四层架构

| 层级 | 目录 | 职责 |
|------|------|------|
| Layer 1 - C++ API | `src/cpp_api/` | `Publisher<T>` / `Subscriber<T>` 模板封装，面向用户 |
| Layer 2 - C API | `src/c_api/` | `dcms_init` / `dcms_msg_publish` / `dcms_msg_subscribe`，元数据填充、CRC 校验、时间戳、路由分发 |
| Layer 3 - Transport | `src/transport/` | IPC 后端接口，Linux 用 Unix Domain Socket，QNX 用 Pulse（当前 stub） |
| Layer 4 - SHM Pool | `src/shm/` | 共享内存池，Buddy + Slab 分配器（当前 malloc stub） |

## 关键特性

- **零拷贝**：消息数据通过共享内存传递指针，IPC 仅传递通知（offset + len），避免内核态拷贝。
- **CRC32 完整性校验**：每条消息携带 CRC32，接收端校验通过后才分发。
- **三时间戳**：publish / system / transfer 三个时间戳用于延迟分析与流量控制。
- **无锁消息队列**：SPSC ring buffer，接收线程写入、用户回调消费。
- **可插拔传输层**：编译期通过 `DCMS_IPC_BACKEND` 选择 `UNIX_SOCKET` 或 `QNX_PULSE`。
- **序列号**：用于去重与丢包检测。
- **队列深度保护**：队列满时返回 `DCMS_ERR_BUSY`，防止内存无限增长。

## 目录结构

```
src/dcms/
├── CMakeLists.txt
├── cmake/dcms_deps.cmake
├── config/dcms_example.json
├── src/
│   ├── dcms_export.h
│   ├── app/main.cpp            # dcms_app 可执行入口（发布/订阅演示）
│   ├── core/                   # DcmsMsgHeader、错误码、CRC32
│   ├── shm/                    # ShmPool + Buddy + Slab
│   ├── transport/              # IpcBackend + Unix/QNX 实现
│   ├── c_api/                  # extern "C" 接口
│   ├── cpp_api/                # Publisher / Subscriber / dcms::init
│   └── runtime/                # RecvThread + MsgQueue
└── third_party/
```

## 构建与运行

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/src/dcms/dcms_app          # 3 秒后 Ctrl+C 或自动退出
```

## 发布流程

```
Publisher::publish(msg)
  -> dcms_msg_publish(topic, data, len)
    -> ShmPool::alloc(header + data)
    -> 填充 DcmsMsgHeader（seq、src/dst addr、type）
    -> CRC32 校验
    -> 3 个时间戳
    -> IpcBackend::sendNotify()
      -> RecvThread 收到通知
        -> CRC 校验
        -> 按 topic 分发到 Subscriber 回调
        -> ShmPool::free()
```

## 测试

```bash
ctest --test-dir build --output-on-failure -R dcms
```
