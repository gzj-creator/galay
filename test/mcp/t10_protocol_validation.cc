/**
 * @file t10_protocol_validation.cc
 * @brief 锁定 MCP 生产策略默认值与生产级错误码 surface。
 */

#include "galay-mcp/common/mcp_error.h"
#include "galay-mcp/common/mcp_policy.h"

#include <chrono>
#include <cstddef>
#include <iostream>
#include <string_view>

using galay::mcp::McpError;
using galay::mcp::McpErrorCode;
using galay::mcp::McpHttpAuthPolicy;
using galay::mcp::McpProductionPolicy;

namespace {

bool require(bool condition, std::string_view message)
{
    if (!condition) {
        std::cerr << message << '\n';
        return false;
    }
    return true;
}

} // namespace

int main()
{
    const McpProductionPolicy policy;

    if (!require(policy.transport.max_stdio_line_bytes >= 1024U * 1024U,
                 "default stdio line limit is too small for existing examples")) {
        return 1;
    }
    if (!require(policy.transport.max_http_body_bytes >= 8U * 1024U * 1024U,
                 "default HTTP body limit is too small for existing examples")) {
        return 1;
    }
    if (!require(policy.transport.max_response_bytes >= 8U * 1024U * 1024U,
                 "default response limit is too small for existing examples")) {
        return 1;
    }
    if (!require(policy.transport.max_in_flight_requests > 0,
                 "default in-flight request limit must be bounded and non-zero")) {
        return 1;
    }
    if (!require(policy.transport.max_keep_alive_requests > 0,
                 "default keep-alive request cap must be bounded and non-zero")) {
        return 1;
    }
    if (!require(policy.timeouts.request_timeout == std::chrono::milliseconds::zero(),
                 "default request timeout should be disabled for backward compatibility")) {
        return 1;
    }
    if (!require(policy.timeouts.initialize_timeout == std::chrono::milliseconds::zero(),
                 "default initialize timeout should be disabled for backward compatibility")) {
        return 1;
    }
    if (!require(!policy.timeouts.requestTimeoutEnabled(),
                 "request timeout helper disagrees with default disabled timeout")) {
        return 1;
    }
    if (!require(!policy.timeouts.initializeTimeoutEnabled(),
                 "initialize timeout helper disagrees with default disabled timeout")) {
        return 1;
    }
    if (!require(policy.http_auth.mode == McpHttpAuthPolicy::Mode::Disabled,
                 "HTTP auth must be disabled by default for backward compatibility")) {
        return 1;
    }
    if (!require(!policy.http_auth.enabled(),
                 "HTTP auth enabled helper disagrees with default disabled mode")) {
        return 1;
    }
    if (!require(!policy.session.strict_http_sessions,
                 "HTTP sessions must remain loose by default for backward compatibility")) {
        return 1;
    }

    if (!require(McpError::timeout().code() == McpErrorCode::Timeout,
                 "timeout error factory returned wrong code")) {
        return 1;
    }
    if (!require(McpError::cancelled().code() == McpErrorCode::Cancelled,
                 "cancelled error factory returned wrong code")) {
        return 1;
    }
    if (!require(McpError::overload().code() == McpErrorCode::Overload,
                 "overload error factory returned wrong code")) {
        return 1;
    }
    if (!require(McpError::unauthorized().code() == McpErrorCode::Unauthorized,
                 "unauthorized error factory returned wrong code")) {
        return 1;
    }
    if (!require(McpError::payloadTooLarge().code() == McpErrorCode::PayloadTooLarge,
                 "payload-too-large error factory returned wrong code")) {
        return 1;
    }

    std::cout << "T10-ProtocolValidationPolicySurface PASS\n";
    return 0;
}
