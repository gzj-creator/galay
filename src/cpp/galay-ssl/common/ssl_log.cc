/**
 * @file ssl_log.cc
 * @brief galay-ssl 独立日志槽实现
 */

#include "ssl_log.h"

#include <utility>

namespace
{
using SslLoggerSlot = ::galay::kernel::LoggerSlot<::galay::ssl::detail::SslLogTag>;
} // namespace

namespace galay::ssl::log
{

/**
 * @brief 设置 galay-ssl 的库级 logger
 * @param logger 日志器
 * @return 无返回值
 */
void set(::galay::kernel::BaseLogger::uptr logger)
{
    SslLoggerSlot::set(std::move(logger));
}

/**
 * @brief 获取 galay-ssl 当前 logger
 * @return ::galay::kernel::BaseLogger* 指针
 */
::galay::kernel::BaseLogger* get() noexcept
{
    return SslLoggerSlot::get();
}

} // namespace galay::ssl::log
