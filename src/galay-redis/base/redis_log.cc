/**
 * @file redis_log.cc
 * @brief galay-redis 独立日志槽实现
 */

#include "galay-redis/base/redis_log.h"

#include <utility>

namespace
{
using RedisLoggerSlot = ::galay::kernel::LoggerSlot<::galay::redis::detail::RedisLogTag>;
} // namespace

namespace galay::redis::log
{

void set(::galay::kernel::BaseLogger::uptr logger)
{
    RedisLoggerSlot::set(std::move(logger));
}

::galay::kernel::BaseLogger* get() noexcept
{
    return RedisLoggerSlot::get();
}

} // namespace galay::redis::log
