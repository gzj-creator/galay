/**
 * @file ssl_log.h
 * @brief galay-ssl 独立日志入口与埋点宏
 * @author galay-ssl
 * @version 1.0.0
 *
 * @details 定义 galay-ssl 库的日志基础设施，包括日志设置/获取接口和
 * SSL_LOG_* 系列宏，每个宏带有 [ssl] 前缀标签。
 */

#ifndef GALAY_SSL_LOG_H
#define GALAY_SSL_LOG_H

#include "galay-kernel/common/log_macro.h"

namespace galay::ssl::detail
{
struct SslLogTag;
} // namespace galay::ssl::detail

namespace galay::ssl::log
{
/**
 * @brief 设置 galay-ssl 的库级 logger
 *
 * @details 只影响 `SSL_LOG_*` 宏产生的日志，不会启用其他 galay 库日志。
 *
 * @param logger 用户自定义 logger；传入 nullptr 时禁用 galay-ssl 日志。
 */
void set(::galay::kernel::BaseLogger::uptr logger);

/**
 * @brief 获取 galay-ssl 当前 logger
 *
 * @return 当前 logger 指针；未设置时返回 nullptr。
 */
[[nodiscard]] ::galay::kernel::BaseLogger* get() noexcept;
} // namespace galay::ssl::log

/// @brief 判断指定级别的 galay-ssl 日志是否会实际写入
#define SSL_LOG_ENABLED(level)                                                   \
    GALAY_LOG_ENABLED(::galay::ssl::log::get, level)

/// @brief galay-ssl 追踪日志宏
#define SSL_LOG_TRACE(tag, ...)                                                  \
    GALAY_LOG_WITH_LOGGER(::galay::ssl::log::get,                                \
                          ::galay::kernel::LogLevel::kTrace, "[ssl] " tag,       \
                          __VA_ARGS__)

/// @brief galay-ssl 调试日志宏
#define SSL_LOG_DEBUG(tag, ...)                                                  \
    GALAY_LOG_WITH_LOGGER(::galay::ssl::log::get,                                \
                          ::galay::kernel::LogLevel::kDebug, "[ssl] " tag,       \
                          __VA_ARGS__)

/// @brief galay-ssl 信息日志宏
#define SSL_LOG_INFO(tag, ...)                                                   \
    GALAY_LOG_WITH_LOGGER(::galay::ssl::log::get,                                \
                          ::galay::kernel::LogLevel::kInfo, "[ssl] " tag,        \
                          __VA_ARGS__)

/// @brief galay-ssl 警告日志宏
#define SSL_LOG_WARN(tag, ...)                                                   \
    GALAY_LOG_WITH_LOGGER(::galay::ssl::log::get,                                \
                          ::galay::kernel::LogLevel::kWarn, "[ssl] " tag,        \
                          __VA_ARGS__)

/// @brief galay-ssl 错误日志宏
#define SSL_LOG_ERROR(tag, ...)                                                  \
    GALAY_LOG_WITH_LOGGER(::galay::ssl::log::get,                                \
                          ::galay::kernel::LogLevel::kError, "[ssl] " tag,       \
                          __VA_ARGS__)

#endif // GALAY_SSL_LOG_H
