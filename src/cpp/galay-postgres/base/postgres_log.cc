#include "postgres_log.h"

#include <utility>

namespace
{
using PostgresLoggerSlot =
    ::galay::kernel::LoggerSlot<::galay::postgres::detail::PostgresLogTag>;
} // namespace

namespace galay::postgres::log
{

void set(::galay::kernel::BaseLogger::uptr logger)
{
    PostgresLoggerSlot::set(std::move(logger));
}

::galay::kernel::BaseLogger* get() noexcept
{
    return PostgresLoggerSlot::get();
}

} // namespace galay::postgres::log
