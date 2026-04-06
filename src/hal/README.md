# HAL - 硬件抽象层

## 模块职责

HAL (Hardware Abstraction Layer) 为上层业务模块提供**统一的传感器接入框架**，屏蔽底层硬件差异。

核心能力：

- **SensorBase 抽象基类**：定义传感器统一生命周期（init / start / stop），子类只需重写 `doInitParams` 实现特定参数解析。
- **传感器子类**：按类型分子目录（camera/、imu/、gnss/、odo/、lidar/），lidar 按具体型号再细分（lidar_pandar128/、lidar_airy/），每个型号包含 driver + sdk_core。
- **SensorFactory（工厂模式）**：按 JSON 中的 `type` + `driver` 字段自动创建对应子类实例，内建注册表支持扩展新传感器。
- **SensorManager（单例模式）**：统一持有所有传感器实例，内含 `Config config_` 成员持有完整配置，提供 `loadFromJson` / `startAll` / `stopAll` / 按 id 查找。
- **utils/Config**：JSON 配置文件加载、解析、校验的通用封装类。
- **utils/LoopThread / SingleThread**：通用线程工具类，可复制到各模块 `src/utils/` 下复用。
- **JSON 配置驱动**：传感器实例化由 `config/` 下的 JSON 文件描述，无需改代码即可增减传感器。

## 目录结构

```
src/hal/
├── CMakeLists.txt
├── cmake/hal_deps.cmake
├── config/
│   ├── sensors_example.json     # 简化示例配置
│   └── hal_online.json          # 完整在线配置
├── src/
│   ├── hal_export.h
│   ├── app/main.cpp             # hal_app 可执行入口
│   ├── sensor/                  # SensorBase + 各传感器子目录
│   │   ├── sensor_base.h/.cpp
│   │   ├── sensor_types.h/.cpp
│   │   ├── camera/
│   │   ├── imu/
│   │   ├── gnss/
│   │   ├── odo/
│   │   └── lidar/
│   │       ├── lidar_base.h/.cpp
│   │       ├── lidar_pandar128/ (driver + sdk_core/)
│   │       └── lidar_airy/      (driver + sdk_core/)
│   ├── factory/                 # SensorFactory
│   ├── manager/                 # SensorManager
│   └── utils/                   # Config, LoopThread, SingleThread
└── third_party/
```

## 构建与运行

```bash
# 在仓库根目录
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/bin/hal_app config/sensors_example.json
```

## 测试

```bash
ctest --test-dir build --output-on-failure -R hal_sensor_smoke
```
