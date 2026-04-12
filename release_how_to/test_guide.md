# 测试指南

本指南说明如何验证以下功能：
- **发布订阅（DCMS pub/sub）**：消息通过共享内存 + 事件通知正确传递
- **日志系统（dlog）**：日志按时间间隔写入 `.log.gz` 文件，可用 `zcat` 查看

> 所有可执行文件均在 `release_how_to/build-x86/bin/` 下，构建方法见 `build_x86`。

---

## 场景一：单进程 pub/sub 自测（最简，单终端）

验证目标：intra-process 模式下发布订阅正确、日志文件正确生成。

```bash
cd release_how_to/build-x86/bin/examples
./pubsub_demo
```

**预期输出**（同时回显到终端）：

```
[20260412_103000][I][MOD:demo] pubsub_demo started
[20260412_103000][I][MOD:demo] dcms initialized
[20260412_103000][I][MOD:demo][publisher] start total=20 interval_ms=200
[20260412_103000][I][MOD:demo][publisher] published counter=1
[20260412_103000][I][MOD:demo][sub_A] recv counter=1 latency_us=... payload=0.000000
[20260412_103000][I][MOD:demo][sub_B] odd counter=1 ...
...
[20260412_103004][I][MOD:demo] finished recv_a=20 recv_b=10 (expected recv_a=20 recv_b=10)
```

**验证日志文件**：

```bash
# 列出生成的 .log.gz 文件（在 ./demo_logs/，相对于运行目录）
ls -lh demo_logs/*.log.gz

# 查看全部日志内容并按时间排序
zcat demo_logs/*.log.gz | sort

# 只看 sub_B 的奇数帧记录
zcat demo_logs/*.log.gz | grep sub_B | sort
```

**预期结果**：
- `recv_a=20`：订阅者 A 收到全部 20 条
- `recv_b=10`：订阅者 B 只处理奇数帧（counter=1,3,5,...,19），共 10 条
- 至少生成 1 个 `.log.gz` 文件（`dlog::shutdown()` 时强制冲刷）

---

## 场景二：跨进程 HAL → 订阅者（三个终端）

验证目标：通过 DCMS broker 在不同进程间传递 LidarFrame，两侧均有 dlog 日志。

```
[Terminal 1] dcms_app        ← broker，负责在进程间转发消息
[Terminal 2] dlog_lidar_subscriber_demo  ← 订阅 lidar 帧的消费者进程
[Terminal 3] hal_app         ← 加载传感器配置，向 broker 发布 fake lidar 数据
```

### 第一步：启动 broker（Terminal 1）

```bash
cd release_how_to/build-x86/bin/dcms
./dcms_app
```

预期输出：
```
[dcms_app] broker listening at /tmp/dcms_main.sock
```

保持此终端运行。

---

### 第二步：启动订阅者进程（Terminal 2）

```bash
cd release_how_to/build-x86/bin/dlog
DCMS_USE_BROKER=1 DCMS_BROKER_SOCKET=/tmp/dcms_main.sock \
    ./dlog_lidar_subscriber_demo 128 30
```

- 第一个参数 `128` 是 sensor id（与 `hal_online.json` 里的 `"id": 128` 一致）
- 第二个参数 `30` 是等待秒数（等待 hal_app 发完数据）

此时订阅者会向 broker 注册，Terminal 1 输出：
```
[dcms_app] subscribe topic=0x... client=/tmp/dcms_sub_xxx.sock
```

订阅者进入等待状态，无其他输出，正常。

---

### 第三步：启动 HAL 发布者（Terminal 3）

```bash
cd release_how_to/build-x86/bin/hal

DCMS_USE_BROKER=1 DCMS_BROKER_SOCKET=/tmp/dcms_main.sock \
    ./hal_app \
    ../../../../src/hal/config/hal_online.json \
    --demo-fake-lidar \
    --demo-seconds=20 \
    --demo-hz=2
```

- `--demo-fake-lidar`：启动内置 UDP 发包线程，模拟真实 lidar 硬件
- `--demo-seconds=20`：发包持续 20 秒
- `--demo-hz=2`：2 Hz（每秒 2 帧），共发 40 帧

---

### 观察各终端输出

**Terminal 1（broker）** 可以看到每帧的转发记录：
```
[dcms_app] publish topic=0x... bytes=... subs=1
```

**Terminal 2（订阅者）** 每收到一帧：
```
[dlog_app] recv frame_id=1001 pts=4 seq=0 capture_ts=... publish_ts=... total=1
[dlog_app] recv frame_id=1002 pts=4 seq=1 ...
...
[dlog_app] done total_recv=40
```

**Terminal 3（hal_app）** 每发送一帧（dlog 同时回显）：
```
[20260412_...][I][MOD:hal_app] fake send sensor_id=128 frame_id=1001 port=2368 sent=1/40
[20260412_...][I][MOD:hal][lidar:128] published frame_id=1001 pts=4 total=1
```

---

### 验证 HAL 日志文件

hal_app 的日志写入 `/tmp/hal_logs/`（每 10s 刷写一次，退出时强制冲刷）：

```bash
ls -lh /tmp/hal_logs/*.log.gz

# 查看全部日志
zcat /tmp/hal_logs/*.log.gz | sort

# 只看 lidar 相关
zcat /tmp/hal_logs/*.log.gz | grep "\[lidar:" | sort

# 按级别过滤（W=warn, E=error）
zcat /tmp/hal_logs/*.log.gz | grep "\]\[W\]\|]\[E\]" | sort
```

---

## 场景三：自动化冒烟测试

直接运行已有的冒烟测试（无需手动启动多个终端）：

```bash
cd release_how_to/build-x86

# DCMS 内部单元测试
./bin/tests/dcms_smoke

# HAL 传感器配置加载测试
./bin/tests/hal_sensor_smoke
```

两者均应以 exit code 0 退出且最后输出 `[PASS]`。

---

## 快速参考：环境变量

| 变量 | 含义 | 默认值 |
|------|------|--------|
| `DCMS_USE_BROKER` | `1` = 使用 Unix Socket broker（跨进程模式）；未设 = intra-process socketpair | 未设 |
| `DCMS_BROKER_SOCKET` | broker 监听的 Unix socket 路径 | `/tmp/dcms_main.sock` |

> `hal_app` 内部默认调用 `enableBrokerDemoByDefault()`，会自动设置这两个变量。
> 其他进程（如 `dlog_lidar_subscriber_demo`）也做了同样的默认设置，因此 **三个进程使用的 socket 路径是一致的**。

---

## 快速参考：日志文件位置

| 进程 | 日志目录 | 刷写间隔 |
|------|----------|----------|
| `pubsub_demo` | `./demo_logs/`（相对于运行目录） | 5s + 退出时强制冲刷 |
| `hal_app` | `/tmp/hal_logs/` | 10s + 退出时强制冲刷 |

日志文件名格式：`dlog_{YYYYMMDD_HHMMSS}_{seq}.log.gz`

读取方式：
```bash
zcat /path/to/*.log.gz          # 展开单个或多个文件
zcat /path/to/*.log.gz | sort   # 按时间戳排序（格式决定字典序 = 时间序）
```

---

## 常见问题

**Q: Terminal 2 等了很久没有收到任何帧**
- 确认 broker（Terminal 1）已经输出了 `broker listening at ...`
- 确认 Terminal 3 的 hal_app 已经输出了 `sensors started`
- 检查 broker 是否有 `publish topic=...` 输出；若无，说明 hal_app 发布失败

**Q: `bind failed` / `address already in use`**
- 上一次运行没有正常退出，socket 文件残留：
  ```bash
  rm -f /tmp/dcms_main.sock
  ```

**Q: `dlog_app: no valid lidar frame received`**
- `dlog_lidar_subscriber_demo` 的 sensor_id 参数与 `hal_online.json` 里的 `"id"` 必须一致
- 当前配置里 id 是 `128`（数字），传参也用 `128`

**Q: 没有生成 `.log.gz` 文件**
- 日志系统是定时刷写的，若进程运行不足 `flush_interval_sec` 且没有正常退出（`dlog::shutdown()`），缓冲区未冲刷
- 正常退出（Ctrl+C 或程序自然结束）会触发 `dlog::shutdown()` 强制冲刷
- 若强制 kill（`kill -9`），最后一批日志会丢失
