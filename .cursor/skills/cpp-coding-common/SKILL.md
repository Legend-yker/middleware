---
name: cpp-coding-common
description: C++ coding conventions for the middleware project. Enforces AUTOSAR-style member naming, module directory layout (common/, api/, utils/, sensor/, manager/, factory/), header placement, and JSON config patterns. Apply when writing or reviewing any C++ code in this repository.
---

# C++ Coding Common Conventions

## Member Variable Naming (AUTOSAR)

All class member variables MUST end with a trailing underscore `_`.

```cpp
class SensorManager {
private:
    Config config_;
    std::vector<std::unique_ptr<SensorBase>> sensors_;
    std::unordered_map<std::string, SensorBase *> id_index_;
};
```

Local variables and function parameters do NOT use trailing underscore.

## Header File Placement

Headers (`.h`) reside in the **same directory** as their corresponding `.cpp`. No separate `include/` tree within a module.

```
src/hal/src/sensor/camera/
├── camera_sensor.h
├── camera_sensor.cpp
└── CMakeLists.txt
```

## Module Directory Layout

Each module under `src/` follows this structure:

```
src/<module>/
├── CMakeLists.txt
├── config/              # JSON configuration files
├── src/
│   ├── api/             # Public headers exposed to other modules
│   ├── common/          # Shared base classes / types within the module
│   ├── utils/           # Utility classes (Config, Thread, HalJson...)
│   ├── factory/         # Factory pattern implementations
│   ├── manager/         # Manager / lifecycle classes
│   ├── sensor/          # (HAL-specific) sensor subdirectories
│   └── app/             # Executable entry points (main.cpp)
└── third_party/
```

Key rules:
- **common/**: base classes and types shared across the module
- **api/**: public-facing headers for inter-module use
- **utils/**: reusable utility classes (can be copied across modules)

## JSON Config Loading

Functions that load from JSON files are named `loadFromJson` (not `loadFromFile`).

```cpp
bool loadFromJson(const std::string &path, std::string &err);
```

## JSON Configuration Format (HAL)

Top-level keys `manager`, `function`, `sensor` are siblings:

```json
{
    "version": "v1",
    "manager": [ ... ],
    "function": [ ... ],
    "sensor": [ ... ]
}
```

## Factory Registration

Sensor drivers register with their JSON `driver_name`:

```cpp
f.registerType("LIDAR_PANDAR128", ...);
f.registerType("CAMERA_NVIDIA", ...);
```

## Naming Conventions

| Element | Convention | Example |
|---------|-----------|---------|
| Class | PascalCase | `SensorManager` |
| Member variable | snake_case + trailing `_` | `config_`, `data_port_` |
| Local variable | snake_case | `sensor_name` |
| Function | camelCase | `loadFromJson`, `startAll` |
| Enum class value | kPascalCase | `SensorType::kLidar` |
| File name | snake_case | `sensor_manager.cpp` |
| Namespace | lowercase | `hal`, `utils` |
| Macro | UPPER_SNAKE | `HAL_API` |
