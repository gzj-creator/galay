#include <galay/cpp/galay-mcp/common/mcp_error.h>
#include <galay/cpp/galay-mcp/common/mcp_base.h>
#include <sstream>

namespace galay::mcp {

std::string McpError::toString() const {
    std::ostringstream oss;
    oss << "McpError[" << static_cast<int>(m_code) << "]: " << m_message;
    if (!m_details.empty()) {
        oss << " (" << m_details << ")";
    }
    return oss.str();
}

int McpError::toJsonRpcErrorCode() const {
    switch (m_code) {
        case McpErrorCode::ParseError:
            return ErrorCodes::PARSE_ERROR;
        case McpErrorCode::InvalidRequest:
            return ErrorCodes::INVALID_REQUEST;
        case McpErrorCode::MethodNotFound:
        case McpErrorCode::InvalidMethod:
            return ErrorCodes::METHOD_NOT_FOUND;
        case McpErrorCode::InvalidTransportMode:
            return ErrorCodes::INVALID_REQUEST;
        case McpErrorCode::Unauthorized:
            return ErrorCodes::INVALID_REQUEST;
        case McpErrorCode::PayloadTooLarge:
            return ErrorCodes::INVALID_REQUEST;
        case McpErrorCode::InvalidParams:
            return ErrorCodes::INVALID_PARAMS;
        case McpErrorCode::InternalError:
        case McpErrorCode::ToolExecutionFailed:
        case McpErrorCode::InitializationFailed:
        case McpErrorCode::ReadError:
        case McpErrorCode::WriteError:
        case McpErrorCode::Timeout:
        case McpErrorCode::Cancelled:
        case McpErrorCode::Overload:
            return ErrorCodes::INTERNAL_ERROR;
        default:
            return ErrorCodes::INTERNAL_ERROR;
    }
}

} // namespace galay::mcp
