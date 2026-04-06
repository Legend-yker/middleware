---
name: cpp-headers-beside-sources
description: 本仓库 C++ 约定：头文件与对应 .cpp 放在同一目录，不使用单独的 include 树（如 src/hal/include）。在处理 middleware 中 HAL 等模块时遵循此布局与包含路径规则。
---

# C++：头文件与源码同目录（middleware）

## 规则

- **禁止**为模块维护独立顶层 `include/<module>/...` 目录承载「对外 API」；**公共头文件与实现放在同一子目录**，与 `.cpp` 同级。
- **包含路径**：在 CMake 中对库设置 `target_include_directories(<target> PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>)`（或等价的单一 `src` 根），源文件中使用与 `src` 相对的路径，例如：
  - `src/sensor/foo.cpp` 与 `src/sensor/foo.h` 互-include 时用 `"foo.h"` 或统一 `"sensor/foo.h"`（推荐全路径一致，便于跨目录引用）。
  - 跨子目录时使用 `"sensor/common/sensor_base.h"`、`"factory/sensor_factory.h"` 等形式。
- **无配对 .cpp 的头文件**（如仅宏的 `hal_export.h`）放在 `src/` 根下或紧邻使用该宏的子目录之一，仍通过上述 `src` 作为唯一对外 include 根对外暴露。

## 与 Agent 的协作方式

- 新增类时：在目标子目录同时添加 `.h` 与 `.cpp`，不要新建 `include/`。
- 修改 CMake 时：不要添加 `include` 子目录到 `target_include_directories`，除非第三方库要求。

## 本仓库示例

- HAL：`src/hal/src/` 为 include 根；可执行入口 `src/hal/src/app/main.cpp`。
