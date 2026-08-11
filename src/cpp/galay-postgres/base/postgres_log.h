/**
 * @file postgres_log.h
 * @brief Module-local PostgreSQL logging entry points.
 */

#ifndef GALAY_POSTGRES_LOG_H
#define GALAY_POSTGRES_LOG_H

#include "../../galay-kernel/common/log_macro.h"

namespace galay::postgres::detail
{
struct PostgresLogTag;
} // namespace galay::postgres::detail

namespace galay::postgres::log
{
void set(::galay::kernel::BaseLogger::uptr logger);
[[nodiscard]] ::galay::kernel::BaseLogger* get() noexcept;
} // namespace galay::postgres::log

#define POSTGRES_LOG_ENABLED(level)                                               \
    GALAY_LOG_ENABLED(::galay::postgres::log::get, level)

#define POSTGRES_LOG_TRACE(tag, ...)                                              \
    GALAY_LOG_WITH_LOGGER(::galay::postgres::log::get,                            \
                          ::galay::kernel::LogLevel::kTrace, "[postgres] " tag,   \
                          __VA_ARGS__)

#define POSTGRES_LOG_DEBUG(tag, ...)                                              \
    GALAY_LOG_WITH_LOGGER(::galay::postgres::log::get,                            \
                          ::galay::kernel::LogLevel::kDebug, "[postgres] " tag,   \
                          __VA_ARGS__)

#define POSTGRES_LOG_INFO(tag, ...)                                               \
    GALAY_LOG_WITH_LOGGER(::galay::postgres::log::get,                            \
                          ::galay::kernel::LogLevel::kInfo, "[postgres] " tag,    \
                          __VA_ARGS__)

#define POSTGRES_LOG_WARN(tag, ...)                                               \
    GALAY_LOG_WITH_LOGGER(::galay::postgres::log::get,                            \
                          ::galay::kernel::LogLevel::kWarn, "[postgres] " tag,    \
                          __VA_ARGS__)

#define POSTGRES_LOG_ERROR(tag, ...)                                              \
    GALAY_LOG_WITH_LOGGER(::galay::postgres::log::get,                            \
                          ::galay::kernel::LogLevel::kError, "[postgres] " tag,   \
                          __VA_ARGS__)

#endif // GALAY_POSTGRES_LOG_H
