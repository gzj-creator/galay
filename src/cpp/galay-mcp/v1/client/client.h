#ifndef GALAY_MCP_CLIENT_CLIENT_H
#define GALAY_MCP_CLIENT_CLIENT_H

#include "../../../galay-http/client/http_client.h"
#include "../../common/mcp_base.h"
#include "../../common/mcp_error.h"
#include "../../../galay-kernel/common/error.h"
#include "../../../galay-kernel/core/runtime.h"

#include <expected>
#include <iostream>
#include <memory>
#include <string>
#include <vector>
#include <utility>


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
    bool tcp_no_delay = true;
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
    CloseAwaitable disconnect_async();
    std::expected<void, McpError> disconnect();

    std::expected<void, McpError> initialize(const std::string& clientName,
                                             const std::string& clientVersion);
    std::expected<std::string, McpError> call_tool(const std::string& toolName,
                                                 const std::string& arguments);
    std::expected<std::vector<Tool>, McpError> list_tools();
    std::expected<std::vector<Resource>, McpError> list_resources();
    std::expected<std::string, McpError> read_resource(const std::string& uri);
    std::expected<std::vector<Prompt>, McpError> list_prompts();
    std::expected<std::string, McpError> get_prompt(const std::string& name,
                                                  const std::string& arguments);
    std::expected<void, McpError> ping();

    galay::kernel::Task<void> initialize(std::string clientName,
                         std::string clientVersion,
                         std::expected<void, McpError>& result);
    galay::kernel::Task<void> call_tool(std::string toolName,
                       std::string arguments,
                       std::expected<std::string, McpError>& result);
    galay::kernel::Task<void> list_tools(std::expected<std::vector<Tool>, McpError>& result);
    galay::kernel::Task<void> list_resources(std::expected<std::vector<Resource>, McpError>& result);
    galay::kernel::Task<void> read_resource(std::string uri,
                           std::expected<std::string, McpError>& result);
    galay::kernel::Task<void> list_prompts(std::expected<std::vector<Prompt>, McpError>& result);
    galay::kernel::Task<void> get_prompt(std::string name,
                        std::string arguments,
                        std::expected<std::string, McpError>& result);
    galay::kernel::Task<void> ping(std::expected<void, McpError>& result);

    bool is_connected() const;
    bool is_initialized() const;
    const ServerInfo& get_server_info() const;
    const ServerCapabilities& get_server_capabilities() const;

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace galay::mcp

#endif
