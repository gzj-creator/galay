/**
 * @file mcp_log.h
 * @brief galay-mcp 独立日志入口与埋点宏
 */

#ifndef GALAY_MCP_LOG_H
#define GALAY_MCP_LOG_H

#include "galay-kernel/common/log_macro.h"

namespace galay::mcp::detail
{
struct McpLogTag;
} // namespace galay::mcp::detail

namespace galay::mcp::log
{
using Slot = ::galay::kernel::LoggerSlot<::galay::mcp::detail::McpLogTag>;

/**
 * @brief 设置 galay-mcp 的库级 logger
 *
 * @details 只影响 `MCP_LOG_*` 宏产生的日志，不会启用 kernel、http
 * 或其他 galay 库的日志。推荐在创建 MCP client/server 之前的
 * 单线程初始化阶段调用。
 *
 * @param logger 用户自定义 logger；传入 nullptr 时禁用 galay-mcp 日志。
 */
void set(::galay::kernel::BaseLogger::uptr logger);

/**
 * @brief 获取 galay-mcp 当前 logger
 *
 * @return 当前 logger 指针；未设置时返回 nullptr。
 *
 * @note 返回指针的生命周期由 `set()` 注入的 unique_ptr 管理，调用方不得释放。
 */
[[nodiscard]] ::galay::kernel::BaseLogger* get() noexcept;
} // namespace galay::mcp::log

/// @brief 判断指定级别的 galay-mcp 日志是否会实际写入
#define MCP_LOG_ENABLED(level)                                                   \
    GALAY_LOG_ENABLED(::galay::mcp::log::get, level)

/// @brief galay-mcp 追踪日志宏，用于最详细的协议调试信息
#define MCP_LOG_TRACE(tag, ...)                                                  \
    GALAY_LOG_WITH_LOGGER(::galay::mcp::log::get,                                \
                          ::galay::kernel::LogLevel::kTrace, "[mcp] " tag,       \
                          __VA_ARGS__)

/// @brief galay-mcp 调试日志宏，用于记录请求/响应和状态转换
#define MCP_LOG_DEBUG(tag, ...)                                                  \
    GALAY_LOG_WITH_LOGGER(::galay::mcp::log::get,                                \
                          ::galay::kernel::LogLevel::kDebug, "[mcp] " tag,       \
                          __VA_ARGS__)

/// @brief galay-mcp 信息日志宏，用于记录连接、初始化和服务生命周期事件
#define MCP_LOG_INFO(tag, ...)                                                   \
    GALAY_LOG_WITH_LOGGER(::galay::mcp::log::get,                                \
                          ::galay::kernel::LogLevel::kInfo, "[mcp] " tag,        \
                          __VA_ARGS__)

/// @brief galay-mcp 警告日志宏，用于表示协议错误、业务错误和可恢复异常
#define MCP_LOG_WARN(tag, ...)                                                   \
    GALAY_LOG_WITH_LOGGER(::galay::mcp::log::get,                                \
                          ::galay::kernel::LogLevel::kWarn, "[mcp] " tag,        \
                          __VA_ARGS__)

/// @brief galay-mcp 错误日志宏，用于表示连接、读写或解析失败
#define MCP_LOG_ERROR(tag, ...)                                                  \
    GALAY_LOG_WITH_LOGGER(::galay::mcp::log::get,                                \
                          ::galay::kernel::LogLevel::kError, "[mcp] " tag,       \
                          __VA_ARGS__)

#endif // GALAY_MCP_LOG_H
