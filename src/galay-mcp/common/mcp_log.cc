/**
 * @file mcp_log.cc
 * @brief galay-mcp 独立日志槽实现
 */

#include "galay-mcp/common/mcp_log.h"

#include <utility>

namespace
{
using McpLoggerSlot = ::galay::kernel::LoggerSlot<::galay::mcp::detail::McpLogTag>;
} // namespace

namespace galay::mcp::log
{

void set(::galay::kernel::BaseLogger::uptr logger)
{
    McpLoggerSlot::set(std::move(logger));
}

::galay::kernel::BaseLogger* get() noexcept
{
    return McpLoggerSlot::get();
}

} // namespace galay::mcp::log
