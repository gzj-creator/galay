/**
 * @file http_log.cc
 * @brief galay-http 独立日志槽实现
 */

#include "http_log.h"

#include <utility>

namespace
{
using HttpLoggerSlot = ::galay::kernel::LoggerSlot<::galay::http::detail::HttpLogTag>;
} // namespace

namespace galay::http::log
{

/**
 * @brief 设置 galay-http 的库级 logger
 * @param logger 日志器
 * @return 无返回值
 */
void set(::galay::kernel::BaseLogger::uptr logger)
{
    HttpLoggerSlot::set(std::move(logger));
}

/**
 * @brief 获取 galay-http 当前 logger
 * @return ::galay::kernel::BaseLogger* 指针
 */
::galay::kernel::BaseLogger* get() noexcept
{
    return HttpLoggerSlot::get();
}

} // namespace galay::http::log
