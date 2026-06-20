/**
 * @file mysql_log.cc
 * @brief galay-mysql 独立日志槽实现
 */

#include <galay/cpp/galay-mysql/base/mysql_log.h>

#include <utility>

namespace
{
using MysqlLoggerSlot = ::galay::kernel::LoggerSlot<::galay::mysql::detail::MysqlLogTag>;
} // namespace

namespace galay::mysql::log
{

void set(::galay::kernel::BaseLogger::uptr logger)
{
    MysqlLoggerSlot::set(std::move(logger));
}

::galay::kernel::BaseLogger* get() noexcept
{
    return MysqlLoggerSlot::get();
}

} // namespace galay::mysql::log
