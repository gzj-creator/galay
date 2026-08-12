/**
 * @file client.h
 * @brief MCP 2026-07-28 clients.
 */

#ifndef GALAY_MCP_V2_CLIENT_H
#define GALAY_MCP_V2_CLIENT_H

#include "protocol.h"
#include "../common/mcp_error.h"
#include "../../galay-http/client/http_client.h"

#include <atomic>
#include <expected>
#include <istream>
#include <memory>
#include <mutex>
#include <ostream>
#include <unordered_map>

namespace galay::mcp::v2 {

struct ClientConfig {
    std::string clientName{"galay-mcp-v2-client"};
    std::string clientVersion{"2.0.0"};
    JsonString clientCapabilities{"{}"};
};

class McpStdioClient {
public:
    McpStdioClient(std::istream& input, std::ostream& output, ClientConfig config = {});

    std::expected<DiscoverResult, McpError> discover();
    std::expected<std::vector<Tool>, McpError> listTools();
    std::expected<JsonString, McpError> callTool(std::string name, JsonString arguments = "{}");
    std::expected<std::vector<Resource>, McpError> listResources();
    std::expected<std::string, McpError> readResource(std::string uri);
    std::expected<std::vector<Prompt>, McpError> listPrompts();
    std::expected<JsonString, McpError> getPrompt(std::string name, JsonString arguments = "{}");

private:
    std::expected<JsonString, McpError> request(std::string_view method,
                                                std::string fields = "{}");
    std::expected<void, McpError> write(std::string_view message);
    std::expected<std::string, McpError> read();
    RequestMeta meta() const;
    std::int64_t nextId() noexcept;

    std::istream* m_input;
    std::ostream* m_output;
    ClientConfig m_config;
    std::atomic<std::int64_t> m_nextId{0};
    std::mutex m_mutex;
};

class McpHttpClient {
public:
    using ConnectAwaitable = decltype(std::declval<http::HttpClient&>().connect(
        std::declval<const std::string&>()));
    using CloseAwaitable = decltype(std::declval<http::HttpClient&>().close());

    McpHttpClient(kernel::Runtime& runtime, std::string url, ClientConfig config = {});
    ConnectAwaitable connect();
    CloseAwaitable close();

    kernel::Task<void> discover(std::expected<DiscoverResult, McpError>& result);
    kernel::Task<void> listTools(std::expected<std::vector<Tool>, McpError>& result);
    kernel::Task<void> callTool(std::string name, JsonString arguments,
                                std::expected<JsonString, McpError>& result);
    kernel::Task<void> listResources(std::expected<std::vector<Resource>, McpError>& result);
    kernel::Task<void> readResource(std::string uri,
                                    std::expected<std::string, McpError>& result);
    kernel::Task<void> listPrompts(std::expected<std::vector<Prompt>, McpError>& result);
    kernel::Task<void> getPrompt(std::string name, JsonString arguments,
                                 std::expected<JsonString, McpError>& result);

private:
    kernel::Task<void> request(std::string method, std::string fields,
                               std::expected<JsonString, McpError>& result);
    RequestMeta meta() const;
    std::int64_t nextId() noexcept;

    kernel::Runtime* m_runtime;
    std::unique_ptr<http::HttpClient> m_client;
    std::string m_url;
    ClientConfig m_config;
    std::atomic<std::int64_t> m_nextId{0};
    std::atomic<bool> m_connected{false};
    std::unordered_map<std::string, Tool> m_toolDefinitions;
    std::mutex m_toolDefinitionsMutex;
};

} // namespace galay::mcp::v2

#endif // GALAY_MCP_V2_CLIENT_H
