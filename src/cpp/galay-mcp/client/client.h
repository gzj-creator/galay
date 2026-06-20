#ifndef GALAY_MCP_CLIENT_CLIENT_H
#define GALAY_MCP_CLIENT_CLIENT_H

#include "../../galay-http/client/http_client.h"
#include "../common/mcp_base.h"
#include "../common/mcp_error.h"
#include "../../galay-kernel/common/error.h"

#include <expected>
#include <iostream>
#include <memory>
#include <string>
#include <vector>
#include <utility>

namespace galay::kernel {
class Runtime;
}

namespace galay::mcp {

enum class McpClientMode {
    Stdio,
    Http
};

struct McpStdioClientConfig {
    std::istream* input = &std::cin;
    std::ostream* output = &std::cout;
};

struct McpHttpClientConfig {
    std::string url;
};

class McpClient {
public:
    using ConnectAwaitable =
        decltype(std::declval<http::HttpClient&>().connect(std::declval<const std::string&>()));
    using CloseAwaitable = decltype(std::declval<http::HttpClient&>().close());

    explicit McpClient(McpStdioClientConfig config = {});
    McpClient(kernel::Runtime& runtime, McpHttpClientConfig config);
    ~McpClient();

    McpClient(const McpClient&) = delete;
    McpClient& operator=(const McpClient&) = delete;
    McpClient(McpClient&&) = delete;
    McpClient& operator=(McpClient&&) = delete;

    McpClientMode mode() const;

    ConnectAwaitable connect();
    ConnectAwaitable connect(std::string url);
    CloseAwaitable disconnectAsync();
    std::expected<void, McpError> disconnect();

    std::expected<void, McpError> initialize(const std::string& clientName,
                                             const std::string& clientVersion);
    std::expected<JsonString, McpError> callTool(const std::string& toolName,
                                                 const JsonString& arguments);
    std::expected<std::vector<Tool>, McpError> listTools();
    std::expected<std::vector<Resource>, McpError> listResources();
    std::expected<std::string, McpError> readResource(const std::string& uri);
    std::expected<std::vector<Prompt>, McpError> listPrompts();
    std::expected<JsonString, McpError> getPrompt(const std::string& name,
                                                  const JsonString& arguments);
    std::expected<void, McpError> ping();

    galay::kernel::Task<void> initialize(std::string clientName,
                         std::string clientVersion,
                         std::expected<void, McpError>& result);
    galay::kernel::Task<void> callTool(std::string toolName,
                       JsonString arguments,
                       std::expected<JsonString, McpError>& result);
    galay::kernel::Task<void> listTools(std::expected<std::vector<Tool>, McpError>& result);
    galay::kernel::Task<void> listResources(std::expected<std::vector<Resource>, McpError>& result);
    galay::kernel::Task<void> readResource(std::string uri,
                           std::expected<std::string, McpError>& result);
    galay::kernel::Task<void> listPrompts(std::expected<std::vector<Prompt>, McpError>& result);
    galay::kernel::Task<void> getPrompt(std::string name,
                        JsonString arguments,
                        std::expected<JsonString, McpError>& result);
    galay::kernel::Task<void> ping(std::expected<void, McpError>& result);

    bool isConnected() const;
    bool isInitialized() const;
    const ServerInfo& getServerInfo() const;
    const ServerCapabilities& getServerCapabilities() const;

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace galay::mcp

#endif
