/**
 * @file tracing_log.h
 * @brief galay-tracing 独立库级日志入口与内部诊断埋点宏
 */

#ifndef GALAY_TRACING_BASE_LOG_H
#define GALAY_TRACING_BASE_LOG_H

#include "galay-kernel/common/log_macro.h"

namespace galay::tracing::detail
{
struct TracingBaseLogTag;
} // namespace galay::tracing::detail

namespace galay::tracing::log
{
using Slot = ::galay::kernel::LoggerSlot<::galay::tracing::detail::TracingBaseLogTag>;

/**
 * @brief 设置 galay-tracing 的库级内部诊断 logger
 *
 * @details 只影响 `TRACING_LOG_*` 宏产生的 tracing 库内部诊断日志，
 * 不会启用 kernel、ssl、http 或其他 galay 库日志，也不会替代 tracing
 * 业务层的 `Logger` / `LogSink` / `GALAY_LOG_*` 关联日志模型。
 * 推荐在创建 span processor 或 exporter 之前的单线程初始化阶段调用。
 *
 * @param logger 用户自定义 logger；传入 nullptr 时禁用 galay-tracing 内部日志。
 */
inline void set(::galay::kernel::BaseLogger::uptr logger)
{
    Slot::set(std::move(logger));
}

/**
 * @brief 获取 galay-tracing 当前内部诊断 logger
 *
 * @return 当前 logger 指针；未设置时返回 nullptr。
 *
 * @note 返回指针的生命周期由 `set()` 注入的 unique_ptr 管理，调用方不得释放。
 */
[[nodiscard]] inline ::galay::kernel::BaseLogger* get() noexcept
{
    return Slot::get();
}
} // namespace galay::tracing::log

/// @brief 判断指定级别的 galay-tracing 内部诊断日志是否会实际写入
#define TRACING_LOG_ENABLED(level)                                               \
    GALAY_LOG_ENABLED(::galay::tracing::log::get, level)

/// @brief galay-tracing 追踪级别内部诊断日志宏
#define TRACING_LOG_TRACE(tag, ...)                                              \
    GALAY_LOG_WITH_LOGGER(::galay::tracing::log::get,                            \
                          ::galay::kernel::LogLevel::kTrace, "[tracing] " tag,   \
                          __VA_ARGS__)

/// @brief galay-tracing 调试级别内部诊断日志宏
#define TRACING_LOG_DEBUG(tag, ...)                                              \
    GALAY_LOG_WITH_LOGGER(::galay::tracing::log::get,                            \
                          ::galay::kernel::LogLevel::kDebug, "[tracing] " tag,   \
                          __VA_ARGS__)

/// @brief galay-tracing 信息级别内部诊断日志宏
#define TRACING_LOG_INFO(tag, ...)                                               \
    GALAY_LOG_WITH_LOGGER(::galay::tracing::log::get,                            \
                          ::galay::kernel::LogLevel::kInfo, "[tracing] " tag,    \
                          __VA_ARGS__)

/// @brief galay-tracing 警告级别内部诊断日志宏
#define TRACING_LOG_WARN(tag, ...)                                               \
    GALAY_LOG_WITH_LOGGER(::galay::tracing::log::get,                            \
                          ::galay::kernel::LogLevel::kWarn, "[tracing] " tag,    \
                          __VA_ARGS__)

/// @brief galay-tracing 错误级别内部诊断日志宏
#define TRACING_LOG_ERROR(tag, ...)                                              \
    GALAY_LOG_WITH_LOGGER(::galay::tracing::log::get,                            \
                          ::galay::kernel::LogLevel::kError, "[tracing] " tag,   \
                          __VA_ARGS__)

/// @brief 兼容早期内部命名，后续代码应优先使用 TRACING_LOG_TRACE
#define TRACING_BASE_LOG_TRACE(tag, ...) TRACING_LOG_TRACE(tag, __VA_ARGS__)

/// @brief 兼容早期内部命名，后续代码应优先使用 TRACING_LOG_DEBUG
#define TRACING_BASE_LOG_DEBUG(tag, ...) TRACING_LOG_DEBUG(tag, __VA_ARGS__)

/// @brief 兼容早期内部命名，后续代码应优先使用 TRACING_LOG_INFO
#define TRACING_BASE_LOG_INFO(tag, ...) TRACING_LOG_INFO(tag, __VA_ARGS__)

/// @brief 兼容早期内部命名，后续代码应优先使用 TRACING_LOG_WARN
#define TRACING_BASE_LOG_WARN(tag, ...) TRACING_LOG_WARN(tag, __VA_ARGS__)

/// @brief 兼容早期内部命名，后续代码应优先使用 TRACING_LOG_ERROR
#define TRACING_BASE_LOG_ERROR(tag, ...) TRACING_LOG_ERROR(tag, __VA_ARGS__)

#endif // GALAY_TRACING_BASE_LOG_H
