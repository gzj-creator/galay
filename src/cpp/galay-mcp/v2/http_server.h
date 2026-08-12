/**
 * @file http_server.h
 * @brief MCP 2026-07-28 Streamable HTTP server.
 */

#ifndef GALAY_MCP_V2_HTTP_SERVER_H
#define GALAY_MCP_V2_HTTP_SERVER_H

#include "protocol.h"
#include "../common/mcp_policy.h"
#include "../../galay-http/server/http_server.h"
#include "../../galay-http/server/http_router.h"

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <shared_mutex>

namespace galay::mcp::v2 {

class McpHttpServer {
public:
    using ToolHandler = std::function<galay::kernel::Task<void>(
        const JsonElement&, std::expected<JsonString, McpError>&)>;
    using ResourceReader = std::function<galay::kernel::Task<void>(
        const std::string&, std::expected<std::string, McpError>&)>;
    using PromptGetter = std::function<galay::kernel::Task<void>(
        const std::string&, const JsonElement&, std::expected<JsonString, McpError>&)>;

    McpHttpServer(std::string host = "127.0.0.1", int port = 8080,
                  std::size_t ioSchedulers = 8, std::size_t computeSchedulers = 0,
                  bool tcpNoDelay = true);
    ~McpHttpServer();
    McpHttpServer(const McpHttpServer&) = delete;
    McpHttpServer& operator=(const McpHttpServer&) = delete;

    void setServerInfo(std::string name, std::string version);
    void setProductionPolicy(McpProductionPolicy policy);
    void addTool(std::string name, std::string description, JsonString inputSchema,
                 ToolHandler handler);
    void addResource(std::string uri, std::string name, std::string description,
                     std::string mimeType, ResourceReader reader);
    void addPrompt(std::string name, std::string description,
                   std::vector<PromptArgument> arguments, PromptGetter getter);
    void start();
    void stop();
    bool isRunning() const noexcept;

private:
    struct ToolEntry { Tool tool; ToolHandler handler; };
    struct ResourceEntry { Resource resource; ResourceReader reader; };
    struct PromptEntry { Prompt prompt; PromptGetter getter; };
    struct HttpResult { int status = 200; JsonString body; };

    galay::kernel::Task<void> process(http::HttpConn& conn, http::HttpRequest& request);
    galay::kernel::Task<void> sendResponse(http::HttpConn& conn, const HttpResult& result);
    galay::kernel::Task<HttpResult> dispatch(http::HttpRequest& request);
    HttpResult error(const std::optional<RequestId>& id, int code, std::string_view message,
                     std::optional<std::string_view> data = std::nullopt,
                     int status = 400) const;
    HttpResult error(const std::optional<RequestId>& id, const McpError& value,
                     int status = 400) const;
    std::expected<void, McpError> validateHeaders(http::HttpRequest& request,
                                                  const ParsedRequest& parsed) const;
    bool validOrigin(http::HttpRequest& request) const;
    std::expected<std::string, McpError> headerName(http::HttpRequest& request,
                                                    const ParsedRequest& parsed) const;

    std::string m_host;
    std::string m_serverName{"galay-mcp-v2-http"};
    std::string m_serverVersion{"2.0.0"};
    std::size_t m_ioSchedulers;
    std::size_t m_computeSchedulers;
    int m_port;
    bool m_tcpNoDelay;
    McpProductionPolicy m_policy;
    std::map<std::string, ToolEntry> m_tools;
    std::map<std::string, ResourceEntry> m_resources;
    std::map<std::string, PromptEntry> m_prompts;
    mutable std::shared_mutex m_registryMutex;
    std::unique_ptr<http::HttpServer> m_httpServer;
    std::unique_ptr<http::HttpRouter> m_router;
    std::atomic<bool> m_running{false};
};

} // namespace galay::mcp::v2

#endif // GALAY_MCP_V2_HTTP_SERVER_H
