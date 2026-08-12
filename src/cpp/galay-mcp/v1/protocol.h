/**
 * @file protocol.h
 * @brief MCP 2024-11-05 现有 API 的显式版本命名空间视图。
 */

#ifndef GALAY_MCP_V1_PROTOCOL_H
#define GALAY_MCP_V1_PROTOCOL_H

#include "../common/json_parser.h"
#include "../common/mcp_base.h"
#include "../common/mcp_error.h"

namespace galay::mcp::v1 {

using ::galay::mcp::ClientInfo;
using ::galay::mcp::Content;
using ::galay::mcp::ContentType;
using ::galay::mcp::InitializeParams;
using ::galay::mcp::InitializeResult;
using ::galay::mcp::JsonRpcError;
using ::galay::mcp::JsonRpcNotification;
using ::galay::mcp::JsonRpcRequest;
using ::galay::mcp::JsonRpcResponse;
using ::galay::mcp::MCP_VERSION;
using ::galay::mcp::Prompt;
using ::galay::mcp::PromptArgument;
using ::galay::mcp::Resource;
using ::galay::mcp::ServerCapabilities;
using ::galay::mcp::ServerInfo;
using ::galay::mcp::Tool;
using ::galay::mcp::ToolCallParams;
using ::galay::mcp::ToolCallResult;
namespace Methods = ::galay::mcp::Methods;
namespace ErrorCodes = ::galay::mcp::ErrorCodes;

} // namespace galay::mcp::v1

#endif // GALAY_MCP_V1_PROTOCOL_H
