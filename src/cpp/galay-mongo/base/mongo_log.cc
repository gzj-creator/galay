/**
 * @file mongo_log.cc
 * @brief galay-mongo 独立日志槽实现
 */

#include <galay/cpp/galay-mongo/base/mongo_log.h>

#include <utility>

namespace
{
using MongoLoggerSlot = ::galay::kernel::LoggerSlot<::galay::mongo::detail::MongoLogTag>;
} // namespace

namespace galay::mongo::log
{

void set(::galay::kernel::BaseLogger::uptr logger)
{
    MongoLoggerSlot::set(std::move(logger));
}

::galay::kernel::BaseLogger* get() noexcept
{
    return MongoLoggerSlot::get();
}

} // namespace galay::mongo::log
