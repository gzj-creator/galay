/**
 * @file log_macro.h
 * @brief galay 系列库通用日志宏
 * @author galay-kernel
 * @version 1.0.0
 *
 * @details 提供零开销的日志埋点宏，供 galay-kernel 和下游库使用。
 * 宏在展开时先通过调用方传入的 getter 获取当前库 logger 裸指针（atomic load），
 * 再检查 minLevel()。两者均通过后才执行 std::format 并调用 log()。
 * 未设置对应库 logger 时，仅执行一次 atomic load + null check，不进入格式化。
 *
 * 各下游库应基于此文件定义自己的命名空间日志宏，
 * 在 tag 前缀中加上模块标识，例如 "[http] [connect]"、"[ssl] [handshake]"。
 *
 * 使用示例：
 * @code
 * // 在 galay-http 中定义
 * #define HTTP_LOG_ERROR(tag, ...)                                              \
 *     GALAY_LOG_WITH_LOGGER(::galay::http::log::get,                            \
 *                           ::galay::kernel::LogLevel::kError,                  \
 *                           "[http] " tag, __VA_ARGS__)
 *
 * // 在库代码中埋点
 * HTTP_LOG_ERROR("[connect] [fail]", "host={}:{} error={}", host, port, err);
 * @endcode
 */

#ifndef GALAY_KERNEL_LOG_MACRO_H
#define GALAY_KERNEL_LOG_MACRO_H

#include "galay-kernel/common/logger.h"

#include <format>
#include <string>

/**
 * @brief 判断指定 getter 和级别下是否会真正写日志
 *
 * @details 供调用点在构造昂贵日志参数前做显式保护。普通 `GALAY_LOG_WITH_LOGGER`
 * 已保证 logger 为空时不执行 std::format；只有参数表达式本身会提前产生明显成本时才需要此宏。
 */
#define GALAY_LOG_ENABLED(getter, level)                                         \
    ([&]() -> bool {                                                             \
        auto* const _galay_log_ptr = getter();                                   \
        return _galay_log_ptr && _galay_log_ptr->minLevel() <= (level);          \
    }())

/**
 * @brief 通用日志宏（核心实现）
 *
 * @details 检查 logger 是否设置且消息级别不低于 minLevel，
 * 通过后使用 std::format 格式化消息并调用 log()。
 * 使用 __builtin_FILE()/__builtin_LINE()/__builtin_FUNCTION()
 * 捕获调用点的源代码位置。
 *
 * @param getter 返回 BaseLogger* 的无参函数，例如 ::galay::ssl::log::get
 * @param level  LogLevel 枚举值
 * @param tag    埋点标签字符串字面量，如 "[http] [connect]"
 * @param fmt    std::format 兼容的格式字符串
 * @param ...    格式化参数（可变）
 *
 * @note 零开销保证：logger 为 nullptr 时，std::format 不会被执行。
 * @note 变量名使用 _galay_ 前缀避免与调用方作用域冲突。
 */
#define GALAY_LOG_WITH_LOGGER(getter, level, tag, fmt, ...)                      \
    do {                                                                         \
        auto* const _galay_log_ptr = getter();                                   \
        if (_galay_log_ptr && _galay_log_ptr->minLevel() <= (level)) {           \
            std::string _galay_log_msg =                                         \
                std::format(fmt __VA_OPT__(,) __VA_ARGS__);                      \
            _galay_log_ptr->log(level, tag, _galay_log_msg,                      \
                                 __builtin_FILE(), __builtin_LINE(),             \
                                 __builtin_FUNCTION());                          \
        }                                                                        \
    } while (0)

/// @brief 判断指定级别的 kernel 日志是否会实际写入
#define GALAY_KERNEL_LOG_ENABLED(level)                                          \
    GALAY_LOG_ENABLED(::galay::kernel::log::get, level)

/// @brief kernel 追踪级别日志宏，用于最详细的开发调试信息
#define GALAY_KERNEL_LOG_TRACE(tag, ...)                                         \
    GALAY_LOG_WITH_LOGGER(::galay::kernel::log::get,                             \
                          ::galay::kernel::LogLevel::kTrace, tag, __VA_ARGS__)

/// @brief kernel 调试级别日志宏，用于排查问题时的上下文信息
#define GALAY_KERNEL_LOG_DEBUG(tag, ...)                                         \
    GALAY_LOG_WITH_LOGGER(::galay::kernel::log::get,                             \
                          ::galay::kernel::LogLevel::kDebug, tag, __VA_ARGS__)

/// @brief kernel 信息级别日志宏，用于记录程序运行的关键事件
#define GALAY_KERNEL_LOG_INFO(tag, ...)                                          \
    GALAY_LOG_WITH_LOGGER(::galay::kernel::log::get,                             \
                          ::galay::kernel::LogLevel::kInfo, tag, __VA_ARGS__)

/// @brief kernel 警告级别日志宏，用于表示潜在问题
#define GALAY_KERNEL_LOG_WARN(tag, ...)                                          \
    GALAY_LOG_WITH_LOGGER(::galay::kernel::log::get,                             \
                          ::galay::kernel::LogLevel::kWarn, tag, __VA_ARGS__)

/// @brief kernel 错误级别日志宏，用于表示操作失败或异常情况
#define GALAY_KERNEL_LOG_ERROR(tag, ...)                                         \
    GALAY_LOG_WITH_LOGGER(::galay::kernel::log::get,                             \
                          ::galay::kernel::LogLevel::kError, tag, __VA_ARGS__)

#endif // GALAY_KERNEL_LOG_MACRO_H
