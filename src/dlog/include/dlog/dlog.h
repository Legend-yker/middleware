#pragma once
// =============================================================================
// dlog — 分布式模块日志系统
//
// 使用方法：
//   1. 进程启动时调用 dlog::init("/path/to/logs", 10)
//      → 后台每 10 秒将缓冲的日志写入 .log.gz 文件
//   2. 各模块用自己的宏写日志：
//      HAL_LOGI("sensor manager started")
//      SLOG_INFO("lidar init ip=" + ip_)  // 在 SensorBase 子类中
//   3. 进程退出前调用 dlog::shutdown() 冲刷剩余日志
//
// 日志格式：
//   [20260412_102536][I][MOD:hal][lidar:57] lidar init success
//    ^^^^^^^^^^^^^^^^^^  ^  ^^^^^^^^^^^^^^  ^^^^^^^^^^^^^^^^^^^
//    时间戳(本地时间)     级别  模块前缀         消息体
// =============================================================================

#include <string>

namespace dlog {

// ---------------------------------------------------------------------------
// 日志级别
// ---------------------------------------------------------------------------
enum class Level : int {
    kDebug = 0,  // D
    kInfo  = 1,  // I
    kWarn  = 2,  // W
    kError = 3,  // E
};

// ---------------------------------------------------------------------------
// 初始化日志系统
//
// output_dir         : .log.gz 文件写入目录（不存在会自动创建）
// flush_interval_sec : 后台多少秒冲刷一次（最小 1s，默认 10s）
// console_echo       : 是否同时输出到 stderr（默认 true）
// ---------------------------------------------------------------------------
void init(const std::string &output_dir    = ".",
          int               flush_interval_sec = 10,
          bool              console_echo   = true);

// 冲刷剩余缓冲并停止后台线程（进程退出前必须调用）
void shutdown();

// ---------------------------------------------------------------------------
// 核心写日志函数（线程安全）
//
// prefix : 模块前缀字符串，例如 "[MOD:hal][lidar:57]"
// msg    : 消息体，例如 "lidar init success"
// ---------------------------------------------------------------------------
void log(Level level, const std::string &prefix, const std::string &msg);

} // namespace dlog

// ---------------------------------------------------------------------------
// 通用宏：供各模块的模块宏内部调用
// ---------------------------------------------------------------------------
#define DLOG_DEBUG(prefix, msg) dlog::log(dlog::Level::kDebug, (prefix), (msg))
#define DLOG_INFO(prefix, msg)  dlog::log(dlog::Level::kInfo,  (prefix), (msg))
#define DLOG_WARN(prefix, msg)  dlog::log(dlog::Level::kWarn,  (prefix), (msg))
#define DLOG_ERROR(prefix, msg) dlog::log(dlog::Level::kError, (prefix), (msg))
