/**
 * @file http_log.cc
 * @brief galay-http 独立日志槽实现
 */

#include "galay-http/common/http_log.h"

#include <utility>

namespace
{
using HttpLoggerSlot = ::galay::kernel::LoggerSlot<::galay::http::detail::HttpLogTag>;
} // namespace

namespace galay::http::log
{

/**
 * @brief 设置 galay-http 的库级 logger
 */
void set(::galay::kernel::BaseLogger::uptr logger)
{
    HttpLoggerSlot::set(std::move(logger));
}

/**
 * @brief 获取 galay-http 当前 logger
 */
::galay::kernel::BaseLogger* get() noexcept
{
    return HttpLoggerSlot::get();
}

} // namespace galay::http::log
