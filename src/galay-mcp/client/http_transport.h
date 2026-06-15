#ifndef GALAY_MCP_CLIENT_HTTP_TRANSPORT_H
#define GALAY_MCP_CLIENT_HTTP_TRANSPORT_H

#include "galay-mcp/client/client_common.h"
#include "galay-http/client/http_client.h"

#include <atomic>
#include <memory>

namespace galay::mcp::detail {

class HttpClientTransport {
public:
    using ConnectAwaitable =
        decltype(std::declval<http::HttpClient&>().connect(std::declval<const std::string&>()));
    using CloseAwaitable = decltype(std::declval<http::HttpClient&>().close());

    explicit HttpClientTransport(kernel::Runtime& runtime, std::string url);

    ConnectAwaitable connect();
    ConnectAwaitable connect(std::string url);
    CloseAwaitable disconnectAsync();

    Coroutine initialize(std::string clientName,
                         std::string clientVersion,
                         std::expected<void, McpError>& result);
    Coroutine callTool(std::string toolName,
                       JsonString arguments,
                       std::expected<JsonString, McpError>& result);
    Coroutine listTools(std::expected<std::vector<Tool>, McpError>& result);
    Coroutine listResources(std::expected<std::vector<Resource>, McpError>& result);
    Coroutine readResource(std::string uri,
                           std::expected<std::string, McpError>& result);
    Coroutine listPrompts(std::expected<std::vector<Prompt>, McpError>& result);
    Coroutine getPrompt(std::string name,
                        JsonString arguments,
                        std::expected<JsonString, McpError>& result);
    Coroutine ping(std::expected<void, McpError>& result);

    bool isConnected() const;
    bool isInitialized() const;
    const ServerInfo& getServerInfo() const;
    const ServerCapabilities& getServerCapabilities() const;

private:
    Coroutine sendRequest(std::string_view method,
                          std::optional<JsonString> params,
                          std::expected<JsonString, McpError>& result);
    int64_t generateRequestId();

private:
    kernel::Runtime* m_runtime;
    std::unique_ptr<http::HttpClient> m_httpClient;
    std::string m_serverUrl;
    std::string m_clientName;
    std::string m_clientVersion;
    ServerInfo m_serverInfo;
    ServerCapabilities m_serverCapabilities;
    std::atomic<bool> m_connected{false};
    std::atomic<bool> m_initialized{false};
    std::atomic<int64_t> m_requestIdCounter{0};
};

} // namespace galay::mcp::detail

#endif
