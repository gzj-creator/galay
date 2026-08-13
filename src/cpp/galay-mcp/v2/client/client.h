/**
 * @file client.h
 * @brief MCP 2026-07-28 clients.
 */

#ifndef GALAY_MCP_V2_CLIENT_H
#define GALAY_MCP_V2_CLIENT_H

#include "../common/protocol.h"
#include "../../common/mcp_error.h"
#include "../../../galay-http/client/http_client.h"

#include <atomic>
#include <expected>
#include <istream>
#include <memory>
#include <ostream>
#include <unordered_map>
#include <functional>

namespace galay::mcp::v2 {

struct ClientConfig {
    std::string clientName{"galay-mcp-v2-client"};
    std::string clientVersion{"2.0.0"};
    JsonString clientCapabilities{"{}"};
};

/**
 * @brief Synchronous, ordered MCP stdio client.
 * @details The input/output streams form one request/response wire. Calls
 *          from different threads are not queued or serialized by blocking;
 *          a concurrent call returns `McpErrorCode::Overload` immediately.
 */
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
    std::atomic_flag m_requestActive{};
};

class McpHttpClient {
public:
    using SubscriptionCallback = std::function<bool(std::string)>;
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
    /**
     * @brief 打开独立的长生命 SSE 订阅流。
     * @param filter 客户端显式 opt-in 的通知过滤器。
     * @param callback 收到每条 JSON 通知时调用，返回 false 表示立即取消。
     * @param result 成功返回服务器确认后接受的过滤器；服务器主动 complete 或连接取消后任务结束。
     */
    kernel::Task<void> listen(SubscriptionFilter filter,
                               SubscriptionCallback callback,
                               std::expected<SubscriptionFilter, McpError>& result);

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
    using ToolDefinitions = std::unordered_map<std::string, Tool>;
    std::atomic<std::shared_ptr<const ToolDefinitions>> m_toolDefinitions{
        std::make_shared<const ToolDefinitions>()};
};

} // namespace galay::mcp::v2

#endif // GALAY_MCP_V2_CLIENT_H
