/**
 * @file mongo_log.h
 * @brief galay-mongo 独立日志入口与埋点宏
 * @author galay-mongo
 * @version 1.0.0
 *
 * @details 提供 galay-mongo 库的日志基础设施，包括：
 * - set()/get()：设置和获取库级 logger
 * - MONGO_LOG_TRACE/DEBUG/INFO/WARN/ERROR：各级别日志宏
 *
 * @note 推荐在创建 Mongo client 之前的单线程初始化阶段调用 set()
 */

#ifndef GALAY_MONGO_LOG_H
#define GALAY_MONGO_LOG_H

#include "../../galay-kernel/common/log_macro.h"

namespace galay::mongo::detail
{
struct MongoLogTag;
} // namespace galay::mongo::detail

namespace galay::mongo::log
{
/**
 * @brief 设置 galay-mongo 的库级 logger
 *
 * @details 只影响 `MONGO_LOG_*` 宏产生的日志，不会启用 kernel、ssl、http
 * 或其他 galay 库日志。推荐在创建 Mongo client 之前的单线程初始化阶段调用。
 *
 * @param logger 用户自定义 logger；传入 nullptr 时禁用 galay-mongo 日志。
 */
void set(::galay::kernel::BaseLogger::uptr logger);

/**
 * @brief 获取 galay-mongo 当前 logger
 *
 * @return 当前 logger 指针；未设置时返回 nullptr。
 *
 * @note 返回指针由 `set()` 注入的 unique_ptr 管理，调用方不得释放。
 */
[[nodiscard]] ::galay::kernel::BaseLogger* get() noexcept;
} // namespace galay::mongo::log

/// @brief 判断指定级别的 galay-mongo 日志是否会实际写入
#define MONGO_LOG_ENABLED(level)                                                 \
    GALAY_LOG_ENABLED(::galay::mongo::log::get, level)

/// @brief galay-mongo 追踪日志宏
#define MONGO_LOG_TRACE(tag, ...)                                                \
    GALAY_LOG_WITH_LOGGER(::galay::mongo::log::get,                              \
                          ::galay::kernel::LogLevel::kTrace, "[mongo] " tag,     \
                          __VA_ARGS__)

/// @brief galay-mongo 调试日志宏
#define MONGO_LOG_DEBUG(tag, ...)                                                \
    GALAY_LOG_WITH_LOGGER(::galay::mongo::log::get,                              \
                          ::galay::kernel::LogLevel::kDebug, "[mongo] " tag,     \
                          __VA_ARGS__)

/// @brief galay-mongo 信息日志宏
#define MONGO_LOG_INFO(tag, ...)                                                 \
    GALAY_LOG_WITH_LOGGER(::galay::mongo::log::get,                              \
                          ::galay::kernel::LogLevel::kInfo, "[mongo] " tag,      \
                          __VA_ARGS__)

/// @brief galay-mongo 警告日志宏
#define MONGO_LOG_WARN(tag, ...)                                                 \
    GALAY_LOG_WITH_LOGGER(::galay::mongo::log::get,                              \
                          ::galay::kernel::LogLevel::kWarn, "[mongo] " tag,      \
                          __VA_ARGS__)

/// @brief galay-mongo 错误日志宏
#define MONGO_LOG_ERROR(tag, ...)                                                \
    GALAY_LOG_WITH_LOGGER(::galay::mongo::log::get,                              \
                          ::galay::kernel::LogLevel::kError, "[mongo] " tag,     \
                          __VA_ARGS__)

#endif // GALAY_MONGO_LOG_H
