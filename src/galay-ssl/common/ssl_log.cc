/**
 * @file ssl_log.cc
 * @brief galay-ssl 独立日志槽实现
 */

#include "galay-ssl/common/ssl_log.h"

#include <utility>

namespace
{
using SslLoggerSlot = ::galay::kernel::LoggerSlot<::galay::ssl::detail::SslLogTag>;
} // namespace

namespace galay::ssl::log
{

/**
 * @brief 设置 galay-ssl 的库级 logger
 */
void set(::galay::kernel::BaseLogger::uptr logger)
{
    SslLoggerSlot::set(std::move(logger));
}

/**
 * @brief 获取 galay-ssl 当前 logger
 */
::galay::kernel::BaseLogger* get() noexcept
{
    return SslLoggerSlot::get();
}

} // namespace galay::ssl::log
