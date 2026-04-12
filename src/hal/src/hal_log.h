#pragma once
// =============================================================================
// hal_log.h — HAL 模块日志宏
//
// 两级宏：
//   1. HAL_LOG* — 模块级别（用于 SensorManager、Config 等非传感器代码）
//      前缀：[MOD:hal]
//
//   2. SLOG_* — 传感器实例级别（在 SensorBase 子类中使用，需要继承 SensorBase）
//      前缀由 SensorBase::slogPrefix() 生成：[MOD:hal][lidar:57]
//      必须在实例方法内调用（使用 this->slogXxx）
//
// 使用示例：
//   // 在 SensorManager 里：
//   HAL_LOGI("sensor manager started count=" + std::to_string(n));
//
//   // 在 Pandar128Driver 里（继承自 SensorBase）：
//   SLOG_INFO("lidar init success ip=" + ip_ + " port=" + std::to_string(data_port_));
//   SLOG_WARN("recv timeout, retrying");
//   SLOG_ERROR("bind failed port=" + std::to_string(data_port_));
// =============================================================================

#include "dlog/dlog.h"
#include <string>

// ---------------------------------------------------------------------------
// 模块级宏：固定前缀 [MOD:hal]
// ---------------------------------------------------------------------------
#define HAL_LOGD(msg) DLOG_DEBUG("[MOD:hal]", (msg))
#define HAL_LOGI(msg) DLOG_INFO ("[MOD:hal]", (msg))
#define HAL_LOGW(msg) DLOG_WARN ("[MOD:hal]", (msg))
#define HAL_LOGE(msg) DLOG_ERROR("[MOD:hal]", (msg))

// ---------------------------------------------------------------------------
// 传感器实例级宏：前缀由 SensorBase 运行时生成，含传感器 ID
// 只能在 SensorBase 子类的成员函数中使用
// ---------------------------------------------------------------------------
#define SLOG_DEBUG(msg) this->slogDebug(msg)
#define SLOG_INFO(msg)  this->slogInfo(msg)
#define SLOG_WARN(msg)  this->slogWarn(msg)
#define SLOG_ERROR(msg) this->slogError(msg)
