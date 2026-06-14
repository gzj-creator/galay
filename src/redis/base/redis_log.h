/**
 * @file redis_log.h
 * @brief galay-redis 独立日志入口与埋点宏
 */

#ifndef GALAY_REDIS_LOG_H
#define GALAY_REDIS_LOG_H

#include "kernel/common/log_macro.h"

namespace galay::redis::detail
{
struct RedisLogTag;
} // namespace galay::redis::detail

namespace galay::redis::log
{
using Slot = ::galay::kernel::LoggerSlot<::galay::redis::detail::RedisLogTag>;

/**
 * @brief 设置 galay-redis 的库级 logger
 *
 * @details 只影响 `REDIS_LOG_*` 宏产生的日志，不会启用 kernel、ssl、http
 * 或其他 galay 库的日志。推荐在创建 Redis 客户端、连接池或会话之前的
 * 单线程初始化阶段调用。
 *
 * @param logger 用户自定义 logger；传入 nullptr 时禁用 galay-redis 日志。
 */
void set(::galay::kernel::BaseLogger::uptr logger);

/**
 * @brief 获取 galay-redis 当前 logger
 *
 * @return 当前 logger 指针；未设置时返回 nullptr。
 *
 * @note 返回指针的生命周期由 `set()` 注入的 unique_ptr 管理，调用方不得释放。
 */
[[nodiscard]] ::galay::kernel::BaseLogger* get() noexcept;
} // namespace galay::redis::log

/// @brief 判断指定级别的 galay-redis 日志是否会实际写入
#define REDIS_LOG_ENABLED(level)                                                 \
    GALAY_LOG_ENABLED(::galay::redis::log::get, level)

/// @brief galay-redis 追踪日志宏，用于最详细的开发调试信息
#define REDIS_LOG_TRACE(tag, ...)                                                \
    GALAY_LOG_WITH_LOGGER(::galay::redis::log::get,                              \
                          ::galay::kernel::LogLevel::kTrace, "[redis] " tag,     \
                          __VA_ARGS__)

/// @brief galay-redis 调试日志宏，用于排查问题时的上下文信息
#define REDIS_LOG_DEBUG(tag, ...)                                                \
    GALAY_LOG_WITH_LOGGER(::galay::redis::log::get,                              \
                          ::galay::kernel::LogLevel::kDebug, "[redis] " tag,     \
                          __VA_ARGS__)

/// @brief galay-redis 信息日志宏，用于记录连接、认证和命令执行等关键事件
#define REDIS_LOG_INFO(tag, ...)                                                 \
    GALAY_LOG_WITH_LOGGER(::galay::redis::log::get,                              \
                          ::galay::kernel::LogLevel::kInfo, "[redis] " tag,      \
                          __VA_ARGS__)

/// @brief galay-redis 警告日志宏，用于表示可恢复异常或潜在问题
#define REDIS_LOG_WARN(tag, ...)                                                 \
    GALAY_LOG_WITH_LOGGER(::galay::redis::log::get,                              \
                          ::galay::kernel::LogLevel::kWarn, "[redis] " tag,      \
                          __VA_ARGS__)

/// @brief galay-redis 错误日志宏，用于表示连接、协议或命令失败
#define REDIS_LOG_ERROR(tag, ...)                                                \
    GALAY_LOG_WITH_LOGGER(::galay::redis::log::get,                              \
                          ::galay::kernel::LogLevel::kError, "[redis] " tag,     \
                          __VA_ARGS__)

/// @brief 兼容致命级别调用点的别名，当前按错误级别写入
#define REDIS_LOG_CRITICAL(tag, ...) REDIS_LOG_ERROR(tag, __VA_ARGS__)

#endif // GALAY_REDIS_LOG_H
