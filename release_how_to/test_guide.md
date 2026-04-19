# 测试指南

本指南说明如何验证重构后 DCMS 中间件的所有核心功能：

- **发布订阅（pub/sub）**：共享内存环形缓冲区 + eventfd 通知
- **RPC（request/reply）**：ServiceServer + ServiceClient 直连调用
- **日志系统（dlog）**：gzip 压缩日志文件，支持多模块同时写入

> 所有可执行文件均在 `build-x86-new/bin/` 下。
> 构建方法见 `release_how_to/build_x86`。

---

## 一、构建

```bash
cd ~/car1609/middleware
mkdir -p build-x86-new && cd build-x86-new
cmake .. -DCMAKE_BUILD_TYPE=RelWithDebInfo
ninja
```

---

## 二、发布订阅测试（dcms_smoke）

### 需要两个终端

**终端 1 — 启动 Daemon（必须先启动）**

```bash
cd ~/car1609/middleware/build-x86-new
./bin/dcms/dcms_daemon
```

预期输出（同时写入日志）：
```
[dcms_daemon] starting on /tmp/dcms_daemon.sock
```

**终端 2 — 运行 pub/sub 冒烟测试**

```bash
cd ~/car1609/middleware/build-x86-new
./bin/tests/dcms_smoke
```

预期输出：
```
[timestamp][I][MOD:dcms_smoke] dcms_smoke start
[timestamp][I][MOD:dcms_smoke] phase1: smoke_pub connected to daemon
[timestamp][I][MOD:dcms_smoke] phase1: smoke_sub connected to daemon
[timestamp][I][MOD:dcms_smoke] phase2: advertised topic smoke/test (8 slots, slot_size=8)
[timestamp][I][MOD:dcms_smoke] phase3: subscribed to smoke/test
[timestamp][I][MOD:dcms_smoke] phase4: published id=42 value=3.14
[timestamp][I][MOD:dcms_smoke] sub_recv: id=42 value=3.140000 total=1
[timestamp][I][MOD:dcms_smoke] phase4: first message received OK
[timestamp][I][MOD:dcms_smoke] phase4: 5 additional messages received, total=6
[timestamp][I][MOD:dcms_smoke] dcms_smoke: all assertions passed
dcms_smoke: ok
```

同时 **终端 1（Daemon）** 会输出：
```
[timestamp][I][MOD:dcms_daemon] node_register: name=smoke_pub pid=... node_id=1
[timestamp][I][MOD:dcms_daemon] node_register: name=smoke_sub pid=... node_id=2
[timestamp][I][MOD:dcms_daemon] advertise: topic=smoke/test shm=/dcms_smoke_test slots=8 slot_size=8
[timestamp][I][MOD:dcms_daemon] subscribe: topic=smoke/test has_publisher=yes
```

---

## 三、RPC 测试（dcms_rpc_smoke）

**不需要 Daemon**，单终端即可运行：

```bash
cd ~/car1609/middleware/build-x86-new
./bin/tests/dcms_rpc_smoke
```

预期输出：
```
[timestamp][I][MOD:dcms_rpc_smoke] dcms_rpc_smoke start
[timestamp][I][MOD:dcms_rpc_smoke] === test_add_service start ===
[timestamp][I][MOD:dcms_rpc_smoke] add(3,4)=7 [OK]
[timestamp][I][MOD:dcms_rpc_smoke] add(-10,5)=-5 [OK]
[timestamp][I][MOD:dcms_rpc_smoke] 10 sequential add calls [OK]
[timestamp][I][MOD:dcms_rpc_smoke] === test_add_service PASS ===
[timestamp][I][MOD:dcms_rpc_smoke] === test_echo_service start ===
[timestamp][I][MOD:dcms_rpc_smoke] echo("hello_dcms_rpc") = "hello_dcms_rpc" [OK]
[timestamp][I][MOD:dcms_rpc_smoke] === test_echo_service PASS ===
[timestamp][I][MOD:dcms_rpc_smoke] non-existent service returns kDcmsErrDaemonNotFound [OK]
[timestamp][I][MOD:dcms_rpc_smoke] === test_nonexistent_service PASS ===
[timestamp][I][MOD:dcms_rpc_smoke] factory add(100,200)=300 [OK]
[timestamp][I][MOD:dcms_rpc_smoke] === test_node_handle_factory PASS ===
[timestamp][I][MOD:dcms_rpc_smoke] dcms_rpc_smoke: all tests passed
dcms_rpc_smoke: ok
```

### RPC 工作原理

```
ServiceServer<Req,Resp>("svc_name")
  → 监听 /tmp/dcms_svc_svc_name.sock

ServiceClient<Req,Resp>("svc_name")
  → call(req, resp): 建立连接 → 发送 [uint32_t len][Req] → 等待 [uint32_t len][Resp]

特点：
  - 不经过 Daemon（零额外延迟）
  - 每次 call() 创建新连接（简单可靠）
  - 支持 timeout_ms 超时参数（默认 1000ms）
  - Req/Resp 必须是 trivially copyable 的 POD 类型
```

---

## 四、日志查询

两个测试都会把日志写入 `/tmp/dcms_logs/`，文件格式为 `dlog_YYYYMMDD_HHMMSS_N.log.gz`。

### 查看所有日志

```bash
zcat /tmp/dcms_logs/*.log.gz | sort
```

### 按模块过滤（关键用法）

```bash
# 只看 Daemon 日志
zcat /tmp/dcms_logs/*.log.gz | grep 'MOD:dcms_daemon'

# 只看 pub/sub 测试日志
zcat /tmp/dcms_logs/*.log.gz | grep 'MOD:dcms_smoke'

# 只看 RPC 测试日志
zcat /tmp/dcms_logs/*.log.gz | grep 'MOD:dcms_rpc_smoke'

# 只看错误和警告
zcat /tmp/dcms_logs/*.log.gz | grep '\]\[E\]\|\]\[W\]'

# 只看订阅者收到的消息
zcat /tmp/dcms_logs/*.log.gz | grep 'sub_recv'
```

### 日志格式说明

```
[20260419_153022][I][MOD:dcms_smoke] phase4: published id=42 value=3.14
 ^^^^^^^^^^^^^^^^ ^  ^^^^^^^^^^^^^^^ ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
 时间戳(本地时间) 级别   模块名称               消息体
```

| 级别字符 | 含义 |
|---------|------|
| `D`     | Debug |
| `I`     | Info |
| `W`     | Warn |
| `E`     | Error |

---

## 五、一键运行所有测试（ctest）

```bash
# 先在后台启动 Daemon（dcms_smoke 需要它）
cd ~/car1609/middleware/build-x86-new
./bin/dcms/dcms_daemon &
DAEMON_PID=$!
sleep 0.5

# 运行所有注册到 ctest 的测试
ctest --output-on-failure

# 结束 Daemon
kill $DAEMON_PID
```

预期输出（3 项全通过）：
```
    Start 1: hal_sensor_smoke
1/3 Test #1: hal_sensor_smoke .........   Passed
    Start 2: dcms_smoke
2/3 Test #2: dcms_smoke ...............   Passed
    Start 3: dcms_rpc_smoke
3/3 Test #3: dcms_rpc_smoke ...........   Passed

100% tests passed, 0 tests failed
```

---

## 六、快速参考

### 进程与 socket 对应关系

| 进程/组件        | 通信方式                                  | socket 路径                         |
|-----------------|------------------------------------------|-------------------------------------|
| `dcms_daemon`   | 监听，供 NodeHandle 连接                  | `/tmp/dcms_daemon.sock`             |
| `NodeHandle`    | 连接 Daemon（注册 / advertise / subscribe）| 同上                               |
| `ServiceServer` | 监听，供 ServiceClient 直连               | `/tmp/dcms_svc_<name>.sock`         |
| `ServiceClient` | 连接 ServiceServer（每次 call 建新连接）   | 同上                               |

### 共享内存

| 步骤              | 内存路径                  | 说明                                  |
|------------------|--------------------------|---------------------------------------|
| Daemon 创建        | `/dcms_<topic>` (shm_open)| `ftruncate` 初始化，不 mmap           |
| Publisher mmap   | 同上                      | PROT_READ\|PROT_WRITE                 |
| Subscriber mmap  | 同上                      | PROT_READ（只读）                     |

### 日志文件

| 进程                | 日志目录            | 刷写间隔               |
|--------------------|---------------------|------------------------|
| `dcms_daemon`       | `/tmp/dcms_logs/`   | 5s + 退出时强制冲刷     |
| `dcms_smoke`        | `/tmp/dcms_logs/`   | 5s + 退出时强制冲刷     |
| `dcms_rpc_smoke`    | `/tmp/dcms_logs/`   | 5s + 退出时强制冲刷     |

所有模块写同一目录，用 `grep MOD:xxx` 区分模块。

---

## 七、常见问题

**Q: dcms_smoke 报 `NodeHandle::create failed` / `Is dcms_daemon running?`**
- Daemon 未启动，或上次未正常退出导致 socket 残留：
  ```bash
  rm -f /tmp/dcms_daemon.sock
  ./bin/dcms/dcms_daemon &
  ```

**Q: dcms_rpc_smoke 某个测试超时（call 返回 kDcmsErrDaemonNotFound）**
- 正常情况不会发生。若发生，检查 `/tmp/dcms_svc_*.sock` 是否有残留：
  ```bash
  rm -f /tmp/dcms_svc_*.sock
  ```

**Q: 没有生成 `.log.gz` 文件**
- dlog 缓冲区每 5 秒刷写一次。若进程运行时间不足 5 秒，在 `dlog::shutdown()` 时会强制冲刷（所有正常退出路径都有）。
- 若被 `kill -9` 强杀，最后一批日志会丢失。使用 `Ctrl+C` 或让程序自然退出。
