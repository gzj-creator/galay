/** @brief MCP 2024-11-05 client API in the explicit v1 namespace. */
#ifndef GALAY_MCP_V1_CLIENT_H
#define GALAY_MCP_V1_CLIENT_H

#include "protocol.h"
#include "../client/client.h"

namespace galay::mcp::v1 {
using ::galay::mcp::McpClient;
using ::galay::mcp::McpClientMode;
using ::galay::mcp::McpHttpClientConfig;
using ::galay::mcp::McpStdioClientConfig;
} // namespace galay::mcp::v1

#endif // GALAY_MCP_V1_CLIENT_H
