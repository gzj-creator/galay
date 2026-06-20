/**
 * @file mysql_log.h
 * @brief galay-mysql 独立日志入口与埋点宏
 */

#ifndef GALAY_MYSQL_LOG_H
#define GALAY_MYSQL_LOG_H

#include "../../galay-kernel/common/log_macro.h"

namespace galay::mysql::detail
{
struct MysqlLogTag;
} // namespace galay::mysql::detail

namespace galay::mysql::log
{
/**
 * @brief 设置 galay-mysql 的库级 logger
 *
 * @details 只影响 `MYSQL_LOG_*` 宏产生的日志，不会启用 kernel、ssl、http
 * 或其他 galay 库日志。推荐在创建 MySQL client/pool 之前的单线程初始化阶段调用。
 *
 * @param logger 用户自定义 logger；传入 nullptr 时禁用 galay-mysql 日志。
 */
void set(::galay::kernel::BaseLogger::uptr logger);

/**
 * @brief 获取 galay-mysql 当前 logger
 *
 * @return 当前 logger 指针；未设置时返回 nullptr。
 *
 * @note 返回指针由 `set()` 注入的 unique_ptr 管理，调用方不得释放。
 */
[[nodiscard]] ::galay::kernel::BaseLogger* get() noexcept;
} // namespace galay::mysql::log

/// @brief 判断指定级别的 galay-mysql 日志是否会实际写入
#define MYSQL_LOG_ENABLED(level)                                                 \
    GALAY_LOG_ENABLED(::galay::mysql::log::get, level)

/// @brief galay-mysql 追踪日志宏
#define MYSQL_LOG_TRACE(tag, ...)                                                \
    GALAY_LOG_WITH_LOGGER(::galay::mysql::log::get,                              \
                          ::galay::kernel::LogLevel::kTrace, "[mysql] " tag,     \
                          __VA_ARGS__)

/// @brief galay-mysql 调试日志宏
#define MYSQL_LOG_DEBUG(tag, ...)                                                \
    GALAY_LOG_WITH_LOGGER(::galay::mysql::log::get,                              \
                          ::galay::kernel::LogLevel::kDebug, "[mysql] " tag,     \
                          __VA_ARGS__)

/// @brief galay-mysql 信息日志宏
#define MYSQL_LOG_INFO(tag, ...)                                                 \
    GALAY_LOG_WITH_LOGGER(::galay::mysql::log::get,                              \
                          ::galay::kernel::LogLevel::kInfo, "[mysql] " tag,      \
                          __VA_ARGS__)

/// @brief galay-mysql 警告日志宏
#define MYSQL_LOG_WARN(tag, ...)                                                 \
    GALAY_LOG_WITH_LOGGER(::galay::mysql::log::get,                              \
                          ::galay::kernel::LogLevel::kWarn, "[mysql] " tag,      \
                          __VA_ARGS__)

/// @brief galay-mysql 错误日志宏
#define MYSQL_LOG_ERROR(tag, ...)                                                \
    GALAY_LOG_WITH_LOGGER(::galay::mysql::log::get,                              \
                          ::galay::kernel::LogLevel::kError, "[mysql] " tag,     \
                          __VA_ARGS__)

#endif // GALAY_MYSQL_LOG_H
