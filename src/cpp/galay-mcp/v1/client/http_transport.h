#ifndef GALAY_MCP_CLIENT_HTTP_TRANSPORT_H
#define GALAY_MCP_CLIENT_HTTP_TRANSPORT_H

#include "client.h"
#include "client_common.h"
#include "../../../galay-http/client/http_client.h"

#include <atomic>
#include <memory>

namespace galay::mcp::detail {

class HttpClientTransport {
public:
    using ConnectAwaitable =
        decltype(std::declval<http::HttpClient&>().connect(std::declval<const std::string&>()));
    using CloseAwaitable = decltype(std::declval<http::HttpClient&>().close());

    explicit HttpClientTransport(kernel::Runtime& runtime, McpHttpClientConfig config);

    ConnectAwaitable connect();
    ConnectAwaitable connect(std::string url);
    CloseAwaitable disconnect_async();

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
    galay::kernel::Task<void> send_request(std::string_view method,
                          std::optional<std::string> params,
                          std::expected<std::string, McpError>& result);
    int64_t generate_request_id();

private:
    kernel::Runtime* m_runtime;
    std::unique_ptr<http::HttpClient> m_httpClient;
    std::string m_serverUrl;
    std::string m_clientName;
    std::string m_clientVersion;
    std::atomic<int64_t> m_requestIdCounter{0};
    ServerInfo m_serverInfo;
    ServerCapabilities m_serverCapabilities;
    std::atomic<bool> m_connected{false};
    std::atomic<bool> m_initialized{false};
};

} // namespace galay::mcp::detail

#endif
