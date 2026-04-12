# DCMS 面试 Q&A

本文档聚焦于面试中最常被追问的实现细节，每个问题都定位到具体代码并配有流程图。

---

## 目录

1. [订阅者怎么知道消息来了？是一直轮询吗？](#q1)
2. [通知机制底层是怎么实现的？](#q2)
3. [订阅者的存储结构是什么？](#q3)
4. [多个模块订阅了同一 topic，内存怎么管理？什么时候释放？](#q4)
5. [多订阅者读取顺序不同，会出问题吗？](#q5)
6. [一个慢订阅者会影响其他订阅者吗？](#q6)
7. [回调是在哪个线程执行的？线程安全吗？](#q7)
8. [dispatch 为什么要先复制订阅列表再调用回调？](#q8)
9. [为什么 socketpair 只传指针就够了？不会出现野指针吗？](#q9)
10. [如果发布方发太快，接收方来不及处理怎么办？](#q10)

---

<a name="q1"></a>
## Q1：订阅者怎么知道消息来了？是一直轮询吗？

**不是轮询，是事件驱动的阻塞等待。**

`RecvThread` 调用 `poll()` 系统调用等待 socket 上有数据，线程被 OS 挂起（不占 CPU），只有 socket 可读时 OS 才唤醒它。

```
RecvThread::loop()                   OS 内核
─────────────────                    ────────
poll(fd, timeout=50ms)  ─────────→  把线程放入等待队列，挂起
                                     （此时线程不占任何 CPU）
                                          │
                                     发布方写入 socket
                                          │
                                     OS 检测到 socket 可读
                                     唤醒 RecvThread ←──────────
│
▼
收到通知，处理消息
```

**关键代码** → `unix_socket_backend.cpp`：

```cpp
// recvNotify 内部，timeout_ms = 50
pollfd pfd{};
pfd.fd     = recv_fd_;
pfd.events = POLLIN;
if (::poll(&pfd, 1, timeout_ms) <= 0)
    return false;   // 超时或出错，返回 false，loop() 继续下一轮
```

**为什么超时设 50ms 而不是永久阻塞？**

```
永久阻塞的问题：
  stop() 设置 running_ = false
  但线程卡在 poll() 里，永远不会检查 running_
  → stop() 永远无法让线程退出

50ms 超时的好处：
  poll() 最多等 50ms，超时返回
  loop() 检查 running_ → false → 退出
  stop() 最多等 50ms 就能 join() 成功
```

---

<a name="q2"></a>
## Q2：通知机制底层是怎么实现的？

分两种模式，但核心都是 **Unix Socket + poll()**。

### socketpair 模式（同进程，默认）

```
初始化时（dcms_init）：
  socketpair(AF_UNIX, SOCK_DGRAM, 0, fds)
  → 创建一对互连的 socket
  → fds[0] 用于发送（发布方）
  → fds[1] 用于接收（RecvThread）

发布时：
  send(fds[0], &{ptr=0x7f3abc00, len=128}, 16字节)

接收时（RecvThread）：
  poll(fds[1], POLLIN, 50ms)   ← OS 阻塞等待
  recv(fds[1], &payload, 16字节)
  ptr = 0x7f3abc00              ← 直接拿到 ShmPool 地址
```

**完整调用链：**

```
dcms_msg_publish()
  └→ IpcBackend::sendNotify(ptr, len)
       └→ UnixSocketBackend::sendNotify()
            └→ ::send(send_fd_, &NotifyPayload{ptr, len}, 16B)
                                    │
                              Unix socket fd
                                    │
                                    ▼
                    RecvThread::loop()
                      └→ backend_->recvNotify()
                           └→ ::poll(recv_fd_, POLLIN, 50ms)  ← 阻塞
                           └→ ::recv(recv_fd_, &payload, 16B)
                           └→ shm_ptr = (void*)payload.ptr    ← 拿到地址
```

### broker 模式（跨进程）

```
进程A（发布方）                  dcms_app（broker）             进程B（订阅方）
──────────────                  ──────────────────             ──────────────
sendto(broker.sock,         →   poll(server_fd, POLLIN)    ←  poll(my_sock, POLLIN)
  [BrokerHeader+Data])          recv → 解析 topic               阻塞等待
                                查 subscribers[topic]
                                for each client:
                                  sendto(client.sock) ──────────→ OS 唤醒进程B
                                                                   recv 完整数据
                                                                   ShmPool::alloc
                                                                   memcpy
                                                                   dispatch
```

---

<a name="q3"></a>
## Q3：订阅者的存储结构是什么？

**一张哈希表，key 是 topic_id，value 是该 topic 下所有订阅者的数组。**

```cpp
// recv_thread.h
struct SubscriptionEntry {
    std::uintptr_t callback_key;  // 函数指针转成整数，用于 unsubscribe 时匹配
    void          *user_key;      // 用户传入的 context 指针（通常是 this）
    MsgCallback    cb;            // std::function<void(hdr, data, len)>
};

std::unordered_map<TopicId, std::vector<SubscriptionEntry>> subs_;
std::mutex sub_mutex_;  // 保护 subs_ 的并发访问
```

**内存布局示意：**

```
subs_
├── topic_id=0x1a2b (lidar/front)
│     └── vector<SubscriptionEntry>
│           ├── [0] cb_key=0xf001, user=0xbeef, cb=λ1  ← 模块A的回调
│           └── [1] cb_key=0xf002, user=0xdead, cb=λ2  ← 模块B的回调
│
├── topic_id=0x3c4d (imu/data)
│     └── vector<SubscriptionEntry>
│           └── [0] cb_key=0xf003, user=0xcafe, cb=λ3
│
└── topic_id=0x5e6f (gnss/fix)
      └── ...
```

**如何通过字符串 topic 名找到正确的 bucket？**

```cpp
// publisher.h / subscriber.h
Publisher pub("lidar/front");
// 等价于：
Publisher pub(static_cast<TopicId>(std::hash<std::string>{}("lidar/front")));
//                                  ↑ C++ 标准哈希，编译器实现，不保证跨平台一致
```

> **面试追问：字符串 hash 会碰撞吗？**
> 会。`std::hash<std::string>` 返回 `size_t`（64位），理论上不同字符串可能产生相同 hash。
> 生产环境建议用枚举常量手动分配 topic_id，完全避免碰撞。

---

<a name="q4"></a>
## Q4：多个模块订阅了同一 topic，内存怎么管理？什么时候释放？

这是最容易出错的地方，需要分清**当前实现**和**理想实现**。

### 当前实现：同步回调 + 数据拷贝

```
RecvThread 收到消息 ptr（ShmPool 中的一块内存）
    │
    ▼
dispatch(hdr):
    复制订阅列表 [λ1, λ2, λ3]
    │
    ├──→ 调用 λ1 (模块A)
    │       └→ Subscriber<T>::cCallback()
    │               T msg;
    │               memcpy(&msg, payload, sizeof(T))  ← 数据拷贝到栈上
    │               用户回调(meta, msg)               ← 用户拿到的是副本
    │               return                            ← 返回后 msg 销毁
    │
    ├──→ 调用 λ2 (模块B)
    │       └→ 同上，独立 memcpy 一份
    │
    └──→ 调用 λ3 (模块C)
            └→ 同上
    │
    ▼
dispatch() 返回（所有回调执行完毕）
    │
    ▼
releaseRecvBuffer(ptr)  →  ShmPool::free(ptr)
```

**关键结论：**

```
┌────────────────────────────────────────────────────────┐
│  内存释放时机 = 所有订阅者的回调都同步执行完毕之后       │
│  每个订阅者拿到的是独立的数据副本（memcpy）              │
│  原始 ShmPool buffer 只有一份，dispatch 结束后立即释放   │
└────────────────────────────────────────────────────────┘
```

**代码定位** → `recv_thread.cpp`：

```cpp
void RecvThread::loop() {
    // ...
    dispatch(hdr);                      // ← 所有回调在这里同步执行完
    backend_->releaseRecvBuffer(ptr);   // ← 之后才释放，绝不提前
}
```

**代码定位** → `subscriber.h`（数据拷贝发生在这里）：

```cpp
static void cCallback(dcms_topic_id, const void *data, uint32_t len, void *user) {
    auto *self = static_cast<Subscriber *>(user);
    const auto *meta = static_cast<const MsgMeta *>(data);
    const void *payload = static_cast<const char *>(data) + sizeof(MsgMeta);
    T msg;
    std::memcpy(&msg, payload, sizeof(T));  // ← 在这里复制！
    self->cb_(*meta, msg);                  // ← 用户拿到的是副本，与 ShmPool 完全解耦
}
```

### 如果想实现真正的零拷贝多播？需要引用计数

当前的"每人复制一份"方案对小消息（几十字节）足够，但对大消息（激光雷达帧，几 MB）效率低。真正的零拷贝多播需要引用计数：

```
发布方分配 buf，ref_count = N（N = 订阅者数量）
    │
    ├──→ 模块A 处理完毕 → ref_count--  → ref_count = 2
    ├──→ 模块B 处理完毕 → ref_count--  → ref_count = 1
    └──→ 模块C 处理完毕 → ref_count--  → ref_count = 0 → ShmPool::free(buf)

规则：最后一个用完的模块负责释放内存
```

这套机制目前**未实现**，是后续迭代的重点。现在用 memcpy 规避了这个复杂性。

---

<a name="q5"></a>
## Q5：多个订阅者读取顺序不同，会出问题吗？

**不会出问题，且顺序是确定的（按注册顺序）。**

原因：所有订阅者的回调在 **同一个** RecvThread 线程里**串行**执行。

```
dispatch() 执行序列（单线程串行）：

时间轴
──────────────────────────────────────────────────────────→
│ 模块A回调 │ 模块B回调 │ 模块C回调 │  releaseRecvBuffer  │
└──────────┘└──────────┘└──────────┘└────────────────────┘
   执行中      排队中      排队中        等所有回调完才释放

每个回调开始执行时，上一个已经完全结束。
没有并发读取，不需要读锁，不会出现数据竞争。
```

**执行顺序由注册顺序决定：**

```cpp
// 先注册先执行
dcms_msg_subscribe(topic, cbA, userA);  // ← cbA 在 vector[0]
dcms_msg_subscribe(topic, cbB, userB);  // ← cbB 在 vector[1]
dcms_msg_subscribe(topic, cbC, userC);  // ← cbC 在 vector[2]

// dispatch 时：for(auto &e : local_subs) → 按下标 0、1、2 顺序调用
```

> **面试追问：每个回调都是独立副本，所以顺序不影响正确性？**
> 是的。因为 `memcpy` 在每个回调内部独立发生，所以即使执行顺序调换，
> 每个模块拿到的数据都是完整独立的，互不干扰。

---

<a name="q6"></a>
## Q6：一个慢订阅者会影响其他订阅者吗？

**会，这是当前架构的已知瓶颈。**

```
时间轴

正常情况（回调都很快）：
──────────────────────────────────────────────────────────→
│消息1：A│消息1：B│消息1：C│ free │消息2：A│消息2：B│ ...
└────────┘└────────┘└────────┘      └────────┘└────────┘

模块B 的回调很慢（比如做了耗时计算）：
──────────────────────────────────────────────────────────→
│消息1：A│  消息1：B（慢！3ms）  │消息1：C│ free │消息2...
└────────┘└─────────────────────┘└────────┘
                 ↑
         这段时间 RecvThread 卡在这里
         新消息堆在 socket buffer 里等待
         所有 topic 的消息处理都被延迟！
```

**根本原因：** 只有 1 个 RecvThread，所有 topic 的消息都在这一个线程里串行分发。

**应对方案（面试常问）：**

| 方案 | 原理 | 代价 |
|------|------|------|
| 回调内不做耗时操作 | 把数据 push 到业务线程自己的队列 | 业务侧多一个线程 |
| 每个 topic 独立线程 | 不同 topic 互不阻塞 | 线程数随 topic 数增长 |
| 线程池分发 | 每条消息提交给线程池 | 需要保证同 topic 的消息顺序 |

当前代码里 `MsgQueue`（`utils/msg_queue.cpp`）就是为"业务侧自己缓冲消息"准备的工具。

---

<a name="q7"></a>
## Q7：回调是在哪个线程执行的？线程安全吗？

**回调在 RecvThread 的工作线程里执行，不在用户的主线程里。**

```
主线程                          RecvThread 工作线程
────────                        ──────────────────
Subscriber<T> sub(topic, cb)    loop()
  → 注册 cb 到 subs_              → recv socket
                                  → dispatch()
                                      → 调用 cb(meta, data)  ← 在这里！
                                         用户代码在这里运行
```

**线程安全要点：**

| 操作 | 保护机制 | 代码位置 |
|------|---------|---------|
| subs_ 读写 | `sub_mutex_` | `recv_thread.cpp: subscribe/unsubscribe/dispatch` |
| seq_counter 递增 | `atomic<uint32_t>` | `dcms_api.cpp: fetch_add` |
| g_ctx 全局状态 | `g_ctx_mutex` | `dcms_api.cpp: init/shutdown/subscribe` |
| ShmPool alloc/free | 内部锁（当前 malloc 自带） | `shm_pool.cpp` |

**用户回调注意事项：**

```cpp
// ❌ 危险：在回调里直接修改主线程的数据，需要加锁
Subscriber<LidarFrame> sub("lidar", [&](auto &meta, auto &data) {
    result_ = data;  // result_ 也被主线程读取 → 数据竞争
});

// ✅ 安全：加锁或用原子操作
Subscriber<LidarFrame> sub("lidar", [&](auto &meta, auto &data) {
    std::lock_guard<std::mutex> lk(result_mutex_);
    result_ = data;
});

// ✅ 更好：把数据推入业务队列，在自己的线程消费
Subscriber<LidarFrame> sub("lidar", [&](auto &meta, auto &data) {
    queue_.push(data);  // 快速返回，不阻塞 RecvThread
});
```

---

<a name="q8"></a>
## Q8：dispatch 为什么要先复制订阅列表再调用回调？

**防止死锁。** 这是一个经典的"持锁调回调"反模式。

```
不复制，直接持锁调回调（错误做法）：

RecvThread（持有 sub_mutex_）          用户回调内
──────────────────────────────         ──────────
lock(sub_mutex_)
for each entry:
    entry.cb(...)  ─────────────────→  my_callback(meta, data) {
                                           // 想在回调里动态订阅
                                           sub2_ = Subscriber<T>("topic2", ...);
                                               → dcms_msg_subscribe()
                                                   → lock(g_ctx_mutex)
                                                   → recv_thread_->subscribe()
                                                       → lock(sub_mutex_)  ← 死锁！
                                                         已被 RecvThread 持有
                                       }

两个线程互相等对方释放锁 → 永远卡住
```

```
复制后再调回调（正确做法，当前实现）：

RecvThread                             用户回调内
──────────────────────────────         ──────────
lock(sub_mutex_)
local_subs = subs_[topic]  ← 复制
unlock(sub_mutex_)          ← 先释放锁！

for each entry in local_subs:          sub2_ = Subscriber<T>(...);
    entry.cb(...)  ─────────────────→      → subscribe()
                                               → lock(sub_mutex_) ← 成功！
                                               → push_back(...)
                                               → unlock
```

**代码定位** → `recv_thread.cpp:82`：

```cpp
void RecvThread::dispatch(const DcmsMsgHeader *hdr) {
    std::vector<SubscriptionEntry> local_subs;
    {
        std::lock_guard<std::mutex> lk(sub_mutex_);
        auto it = subs_.find(hdr->topic_id);
        if (it == subs_.end()) return;
        local_subs = it->second;   // ← 复制
    }                              // ← 锁在这里释放，花括号作用域结束
    // 此后调用回调时不持任何锁
    const void *data = msgDataPtr(hdr);
    for (auto &entry : local_subs)
        entry.cb(hdr, data, hdr->data_len);
}
```

> **面试追问：复制有开销，如何优化？**
> 订阅者数量通常很少（个位数），`vector` 的浅拷贝开销极小（复制几个指针）。
> 如果订阅者非常多，可以改用 `shared_ptr<const vector<...>>` 做 COW（写时复制）。

---

<a name="q9"></a>
## Q9：socketpair 只传指针就够了？不会出现野指针吗？

**不会，有严格的所有权保证。**

```
发布方                                  接收方（RecvThread）
──────                                  ──────────────────

① ShmPool::alloc() → buf = 0x7f3abc00
② 填写 header + data
③ sendNotify({ptr=0x7f3abc00, len=128})
④ usesSharedMemoryTransport() == true
   → 不释放 buf！                     ⑤ recvNotify() 收到 ptr
                                       ⑥ 验证 CRC
   发布方"放弃所有权"                   ⑦ dispatch()（回调执行完）
   这块内存不能再访问                   ⑧ releaseRecvBuffer(ptr)
                                           → ShmPool::free(ptr)
                                           内存在这里释放
```

**所有权转移规则：**

```
sendNotify 成功后：
  发布方  → 放弃所有权，不能再访问 buf
  接收方  → 获得所有权，负责最终释放

保证条件（代码中明确）：
  dcms_api.cpp:
    if (!g_ctx.ipc->usesSharedMemoryTransport())  // broker 模式
        ShmPool::instance().free(buf);             // broker 模式下发布方释放
    // socketpair 模式：不执行 free，所有权已转给接收方
```

**发布方发完之后还能访问 buf 吗？**

```
❌ 不能！示例代码（错误）：
    void *buf = ShmPool::alloc(size);
    fill(buf);
    ipc->sendNotify(buf, size);
    read_from(buf);  ← 未定义行为！接收方可能已经 free 了
```

> **面试追问：如果发布方和接收方同时访问呢？**
> socketpair 模式下：发完 sendNotify() 就必须放弃所有权。
> 收到 `sendNotify()` 返回 true 意味着消息已入队，接收方随时可能读取。
> 这是一个约定（contract），而非用锁强制。违反约定会出现未定义行为。

---

<a name="q10"></a>
## Q10：发布方发太快，接收方来不及处理怎么办？

**当前方案：socket 缓冲区满时 `sendNotify` 返回失败，上层报 `kDcmsErrBusy`。**

```
发布方 send() 调用链：

sendNotify()
  └→ ::send(sock_fd, &payload, 16B, MSG_NOSIGNAL)
      │
      ├── socket 内核缓冲区有空间 → 成功，返回 16
      │
      └── 内核缓冲区满（接收方处理慢）
           MSG_NOSIGNAL + SOCK_DGRAM → 立即返回 EAGAIN（-1）
           sendNotify 返回 false
           dcms_msg_publish 返回 kDcmsErrBusy

上层处理（dcms_api.cpp）：
    if (!g_ctx.ipc->sendNotify(buf, total_size)) {
        ShmPool::instance().free(buf);  // 消息丢弃，释放内存
        return dcms::kDcmsErrBusy;      // 告知调用者"丢包了"
    }
```

**发送侧的背压策略对比：**

```
策略           │ 当前实现         │ 描述
───────────────┼──────────────────┼──────────────────────────────
丢弃最新消息   │ ✅ 是（默认）    │ 返回 kDcmsErrBusy，最新包丢弃
丢弃最旧消息   │ ❌ 未实现        │ 需要循环 buffer，踢出旧消息
阻塞发布方     │ ❌ 未实现        │ 去掉 MSG_NOSIGNAL 改用阻塞 send
限速（令牌桶） │ ❌ 未实现        │ 在 C++ API 层加 rate limiter
```

**DcmsMsgHeader 中的 seq 字段可用于丢包检测：**

```cpp
// 接收方检测丢包（用户代码示例）
uint32_t last_seq = 0;
Subscriber<T> sub("topic", [&](auto &meta, auto &data) {
    if (meta.seq != last_seq + 1) {
        uint32_t dropped = meta.seq - last_seq - 1;
        printf("丢了 %u 包！\n", dropped);
    }
    last_seq = meta.seq;
});
```

---

## 快速索引：问题 → 代码位置

| 问题 | 文件 | 关键行 |
|------|------|--------|
| poll 阻塞等待 | `transport/unix_socket/unix_socket_backend.cpp` | `::poll(&pfd, 1, timeout_ms)` |
| 50ms 超时退出 | `manager/recv_thread.cpp` | `loop()` 中 `if (!backend_->recvNotify(..., 50))` |
| 订阅表结构 | `manager/recv_thread.h` | `subs_` 字段 |
| 回调注册 | `manager/recv_thread.cpp` | `subscribe()` |
| 所有回调执行完再 free | `manager/recv_thread.cpp` | `loop()` 中 `dispatch()` 后紧跟 `releaseRecvBuffer()` |
| 数据拷贝（每人一份） | `api/subscriber.h` | `cCallback()` 中 `memcpy` |
| 复制列表防死锁 | `manager/recv_thread.cpp` | `dispatch()` 中先 `local_subs = it->second` 再解锁 |
| 发布方放弃所有权 | `api/dcms_api.cpp` | `if (!usesSharedMemoryTransport()) free(buf)` |
| 背压（丢包） | `api/dcms_api.cpp` | `sendNotify` 失败后 `free + return kDcmsErrBusy` |
| topic 字符串 hash | `api/publisher.h` | `std::hash<std::string>{}(topic_name)` |
