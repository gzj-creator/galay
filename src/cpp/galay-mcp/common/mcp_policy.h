/**
 * @file mcp_policy.h
 * @brief MCP生产运行策略值类型
 * @author galay-mcp
 * @version 1.0.0
 *
 * @details 定义MCP stdio/HTTP传输的生产级默认限制、超时、会话和认证策略。
 *          本文件只提供值类型，不持有网络、线程、协程或外部资源。
 */

#ifndef GALAY_MCP_COMMON_MCPPOLICY_H
#define GALAY_MCP_COMMON_MCPPOLICY_H

#include "mcp_error.h"

#include <chrono>
#include <cstddef>
#include <expected>
#include <functional>
#include <string>
#include <string_view>

namespace galay::mcp {

/**
 * @brief MCP传输层资源限制
 * @details 默认值保持向后兼容：限制是有界的，但足够容纳当前示例和常规工具结果。
 */
struct McpTransportLimits {
    std::size_t max_stdio_line_bytes = 8U * 1024U * 1024U; ///< 单条stdio消息最大字节数
    std::size_t max_http_body_bytes = 32U * 1024U * 1024U; ///< 单个HTTP请求体最大字节数
    std::size_t max_response_bytes = 32U * 1024U * 1024U; ///< 单个响应最大字节数
    std::size_t max_in_flight_requests = 1024; ///< 服务端同时处理的最大请求数
    std::size_t max_keep_alive_requests = 1000; ///< 单个HTTP keep-alive连接最大请求数
};

/**
 * @brief MCP请求超时策略
 * @details 0ms表示禁用对应超时，用于保持默认行为兼容；启用后由具体传输实现解释。
 */
struct McpTimeoutPolicy {
    std::chrono::milliseconds request_timeout{0}; ///< 普通请求超时，0表示禁用
    std::chrono::milliseconds initialize_timeout{0}; ///< initialize请求超时，0表示禁用

    bool requestTimeoutEnabled() const noexcept { return request_timeout > std::chrono::milliseconds::zero(); }
    bool initializeTimeoutEnabled() const noexcept { return initialize_timeout > std::chrono::milliseconds::zero(); }
};

/**
 * @brief MCP HTTP会话策略
 * @details 默认关闭严格会话隔离，以保留现有HTTP示例的无会话行为。
 */
struct McpSessionPolicy {
    std::chrono::milliseconds idle_timeout{0}; ///< 会话空闲超时，0表示禁用
    std::chrono::milliseconds max_age{0}; ///< 会话最大生命周期，0表示禁用
    bool strict_http_sessions = false; ///< true时要求initialize后使用显式session id
};

/**
 * @brief MCP HTTP认证策略
 * @details 只描述MCP层的Authorization检查策略，不处理TLS证书或身份提供方流程。
 */
struct McpHttpAuthPolicy {
    enum class Mode {
        Disabled, ///< 不检查Authorization头
        BearerToken, ///< 使用固定Bearer token
        Callback ///< 使用调用方提供的验证回调
    };

    using Validator = std::function<std::expected<void, McpError>(std::string_view authorization_header)>;

    std::string bearer_token; ///< BearerToken模式下期望的token值
    Validator validator; ///< Callback模式下的认证回调
    Mode mode = Mode::Disabled; ///< 默认关闭认证，保持向后兼容

    bool enabled() const noexcept { return mode != Mode::Disabled; }
};

/**
 * @brief MCP生产运行总策略
 * @details 组合传输限制、超时、HTTP会话和HTTP认证策略；默认构造保持兼容模式。
 */
struct McpProductionPolicy {
    McpTransportLimits transport; ///< 传输资源限制
    McpTimeoutPolicy timeouts; ///< 请求超时策略
    McpSessionPolicy session; ///< HTTP会话策略
    McpHttpAuthPolicy http_auth; ///< HTTP认证策略
};

} // namespace galay::mcp

#endif // GALAY_MCP_COMMON_MCPPOLICY_H
