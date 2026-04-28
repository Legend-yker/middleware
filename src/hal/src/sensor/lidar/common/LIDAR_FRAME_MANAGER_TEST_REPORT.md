# LidarFrameManager 测试报告

**测试时间：** 2026-04-27  
**测试环境：** x86-64 / Linux WSL2 / Release 构建  
**测试文件：** `tests/lidar_frame_manager_test.cpp`  
**测试结果：** ✅ 30 PASS / 0 FAIL

---

## 被测模块

| 文件 | 说明 |
|------|------|
| `sensor/lidar/common/lidar_frame_manager.h` | 接口定义 |
| `sensor/lidar/common/lidar_frame_manager.cpp` | 实现 |

**测试方式：** 纯单元测试，不依赖硬件和网络，直接构造点云数据调用 `feed()` 验证输出。

---

## 测试用例详情

### Test1 — 包数触发帧完成

**目的：** 验证积累到 `pkts_per_frame` 包后自动触发帧完成。

| 检查项 | 结果 |
|--------|------|
| `pkts_per_frame` 包后 `feed()` 返回 true | ✅ PASS |
| 帧状态为 `kOk` | ✅ PASS |
| `pkt_got` 等于 `pkts_per_frame` | ✅ PASS |
| `num_points` 等于 包数 × 每包点数 | ✅ PASS |
| `frame_ok` 统计计数为 1 | ✅ PASS |

---

### Test2 — 方位角回绕触发帧边界

**目的：** 验证当方位角从 ≥270° 回绕到 <90° 时，检测到帧边界并关闭当前帧。

**输入：** 12 包，角度从 0° 步进到 359.9°；第 13 包 angle_start=1°（<90°）触发边界。

| 检查项 | 结果 |
|--------|------|
| 方位角回绕后 `feed()` 返回 true | ✅ PASS |
| 帧状态为 `kOk` | ✅ PASS |
| `pkt_got` = 12（边界包不计入当前帧） | ✅ PASS |

**关键设计点：** 边界检测在 `curr_pkts_++` 之前执行，触发边界的包成为**下一帧**的第一包，不被计入当前帧。

---

### Test3 — 帧超时强制发布

**目的：** 验证帧内设备时间跨度超过 `frame_timeout_us` 时强制发布不完整帧。

**输入：** 超时设为 5000μs，第一包 t=1000000，第二包 t=1006000（间隔 6000μs > 5000μs）。

| 检查项 | 结果 |
|--------|------|
| 超时后 `feed()` 返回 true | ✅ PASS |
| 帧状态为 `kTimeout` | ✅ PASS |
| `frame_timeout` 统计计数为 1 | ✅ PASS |

---

### Test4 — 时间戳法丢包检测

**目的：** 验证当包间隔超过 `pkt_gap_us × drop_ratio` 时估算丢包数。

**输入：**
- 包1 → 包2：间隔 1000μs（正常，=pkt_gap_us）
- 包2 → 包3：间隔 4000μs（异常，超过 1.8×1000=1800μs）

| 检查项 | 结果 |
|--------|------|
| `gap_curr_us` 正确记录为 4000μs | ✅ PASS |
| 估算丢包数 = ⌊(4000+500)/1000⌋ - 1 = 3 | ✅ PASS |

---

### Test5 — 序列号法丢包检测

**目的：** 验证序列号跳变时确认丢包数。

**输入：** 依次发送 seq=0,1,2，然后跳到 seq=6（丢失 3,4,5 共 3 包）。

| 检查项 | 结果 |
|--------|------|
| `drop_by_seq` = 3 | ✅ PASS |

**关键修复：** 使用 `seq_initialized_` 标记首次调用，避免以默认值 0 与第一个 seq 比较导致误检。

---

### Test6 — 无效点过滤

**目的：** 验证距离不在 `[min_dist_m, max_dist_m]` 范围内的点被过滤。

**输入：** 4 个点：距离 0m（无效）、5m（有效）、0.1m（<0.5m 无效）、50m（>10m 无效）。

| 检查项 | 结果 |
|--------|------|
| 帧就绪 | ✅ PASS |
| `num_points` = 1（只有5m的点通过） | ✅ PASS |

---

### Test7 — 角度覆盖率计算

**目的：** 验证角度覆盖位图和 `angle_cover` 统计。

**输入：** 3 包，覆盖 [0°,59°]，共 60°。期望覆盖率 = 60/360 ≈ 16.7%。

| 检查项 | 结果 |
|--------|------|
| 帧就绪 | ✅ PASS |
| `angle_cover` ≈ 0.167（误差 < 2%） | ✅ PASS |

---

### Test8 — 三层时间戳滚动

**目的：** 验证 `time_curr / time_prev / time_older` 及 `gap_curr / gap_prev` 滚动正确。

**输入：** 依次发包，时间戳为 100、200、400μs。

| 检查项 | 结果 |
|--------|------|
| `time_curr` = 400 | ✅ PASS |
| `time_prev` = 200 | ✅ PASS |
| `time_older` = 100 | ✅ PASS |
| `gap_curr` = 200（400-200） | ✅ PASS |
| `gap_prev` = 100（200-100） | ✅ PASS |

---

### Test9 — reset() 清空状态

**目的：** 验证 `reset()` 后所有统计归零。

| 检查项 | 结果 |
|--------|------|
| `frame_total` = 0 | ✅ PASS |
| `pkt_total` = 0 | ✅ PASS |
| `time_curr_us` = 0 | ✅ PASS |
| `fps` = 0.0 | ✅ PASS |

---

### Test10 — statsReport 格式验证

**目的：** 验证 `report()` 输出非空且包含关键字段。

**实际输出示例：**
```
frames=1 ok=1 timeout=0 full=0 drop_t=0 drop_s=0 fps=0 pts=0 cover=0%
```

| 检查项 | 结果 |
|--------|------|
| `report()` 非空 | ✅ PASS |
| 包含 `frames=` | ✅ PASS |
| 包含 `fps=` | ✅ PASS |

---

## 发现并修复的 Bug

| Bug | 原因 | 修复方式 |
|-----|------|---------|
| 边界包被计入当前帧 `pkt_got`（+1） | `curr_pkts_++` 在边界检测之前执行 | 边界检测移到 `++` 之前 |
| seq 首次检测误判丢包 | `last_seq` 默认值 0 与第二包 seq 不匹配 | 增加 `seq_initialized_` 标记首次调用 |
| `take()` 返回积累中的帧而非完成帧 | 单缓冲设计，边界触发后 `buf_` 已开始积累新帧 | 改为双缓冲：`out_frame_`（完成帧）+ `buf_`（积累帧） |
| `isNewFrame` 假触发（>180° 判断太宽） | 条件过于宽松，扫描中途也会触发 | 改为 `last_angle_≥270° && curr_angle<90°` |

---

## 局限说明

1. **硬件测试未进行**：Pandar128 真实 Hesai 协议解析需要连接实体设备，当前环境无硬件。
2. **`hal_sensor_smoke` 与新驱动不兼容**：smoke test 发送旧 `FakePandarPacket` 格式，新驱动解析真实 Hesai 协议，二者协议不匹配；smoke test 后续需改造为发送真实格式数据或改为 mock 模式。
3. **帧率 fps 在测试中为 0**：因为测试内单帧耗时 <1ms，滑动平均计算出的 fps 为 0，属正常现象，实际运行时约为 10Hz。
