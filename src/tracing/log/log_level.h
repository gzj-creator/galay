/**
 * @file log_level.h
 * @brief 日志级别枚举与名称转换
 * @author galay-tracing
 * @version 1.0.0
 *
 * @details 定义 galay-tracing 日志系统的日志级别枚举，
 * 以及将日志级别转换为可读字符串的函数。
 */

#pragma once

#include <string_view>

namespace galay::tracing {

/**
 * @brief 日志级别枚举
 */
enum class LogLevel {
    kTrace = 0,  ///< 追踪级别，最详细的日志
    kDebug = 1,  ///< 调试级别
    kInfo = 2,   ///< 信息级别
    kWarn = 3,   ///< 警告级别
    kError = 4,  ///< 错误级别
    kOff = 5,    ///< 禁用所有日志
};

/**
 * @brief 获取日志级别的可读名称
 * @param level 日志级别
 * @return 级别名称的字符串视图（如 "TRACE"、"DEBUG" 等）
 */
[[nodiscard]] constexpr std::string_view logLevelName(LogLevel level) noexcept {
    switch (level) {
    case LogLevel::kTrace:
        return "TRACE";
    case LogLevel::kDebug:
        return "DEBUG";
    case LogLevel::kInfo:
        return "INFO";
    case LogLevel::kWarn:
        return "WARN";
    case LogLevel::kError:
        return "ERROR";
    case LogLevel::kOff:
        return "OFF";
    }
    return "UNKNOWN";
}

} // namespace galay::tracing
