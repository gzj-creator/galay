/**
 * @file http_log.h
 * @brief galay-http 独立日志入口与埋点宏
 */

#ifndef GALAY_HTTP_LOG_H
#define GALAY_HTTP_LOG_H

#include "galay-kernel/common/log_macro.h"

namespace galay::http::detail
{
struct HttpLogTag;
} // namespace galay::http::detail

namespace galay::http::log
{
/**
 * @brief 设置 galay-http 的库级 logger
 *
 * @details 只影响 `HTTP_LOG_*` 宏产生的日志，不会启用其他 galay 库日志。
 *
 * @param logger 用户自定义 logger；传入 nullptr 时禁用 galay-http 日志。
 */
void set(::galay::kernel::BaseLogger::uptr logger);

/**
 * @brief 获取 galay-http 当前 logger
 *
 * @return 当前 logger 指针；未设置时返回 nullptr。
 */
[[nodiscard]] ::galay::kernel::BaseLogger* get() noexcept;
} // namespace galay::http::log

/// @brief 判断指定级别的 galay-http 日志是否会实际写入
#define HTTP_LOG_ENABLED(level)                                                  \
    GALAY_LOG_ENABLED(::galay::http::log::get, level)

/// @brief galay-http 追踪日志宏
#define HTTP_LOG_TRACE(tag, ...)                                                 \
    GALAY_LOG_WITH_LOGGER(::galay::http::log::get,                               \
                          ::galay::kernel::LogLevel::kTrace, "[http] " tag,      \
                          __VA_ARGS__)

/// @brief galay-http 调试日志宏
#define HTTP_LOG_DEBUG(tag, ...)                                                 \
    GALAY_LOG_WITH_LOGGER(::galay::http::log::get,                               \
                          ::galay::kernel::LogLevel::kDebug, "[http] " tag,      \
                          __VA_ARGS__)

/// @brief galay-http 信息日志宏
#define HTTP_LOG_INFO(tag, ...)                                                  \
    GALAY_LOG_WITH_LOGGER(::galay::http::log::get,                               \
                          ::galay::kernel::LogLevel::kInfo, "[http] " tag,       \
                          __VA_ARGS__)

/// @brief galay-http 警告日志宏
#define HTTP_LOG_WARN(tag, ...)                                                  \
    GALAY_LOG_WITH_LOGGER(::galay::http::log::get,                               \
                          ::galay::kernel::LogLevel::kWarn, "[http] " tag,       \
                          __VA_ARGS__)

/// @brief galay-http 错误日志宏
#define HTTP_LOG_ERROR(tag, ...)                                                 \
    GALAY_LOG_WITH_LOGGER(::galay::http::log::get,                               \
                          ::galay::kernel::LogLevel::kError, "[http] " tag,      \
                          __VA_ARGS__)

#endif // GALAY_HTTP_LOG_H
