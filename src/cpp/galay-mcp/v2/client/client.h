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
    std::string clientCapabilities{"{}"};
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
    std::expected<std::vector<Tool>, McpError> list_tools();
    std::expected<std::string, McpError> call_tool(std::string name, std::string arguments = "{}");
    std::expected<std::vector<Resource>, McpError> list_resources();
    std::expected<std::string, McpError> read_resource(std::string uri);
    std::expected<std::vector<Prompt>, McpError> list_prompts();
    std::expected<std::string, McpError> get_prompt(std::string name, std::string arguments = "{}");

private:
    std::expected<std::string, McpError> request(std::string_view method,
                                                std::string fields = "{}");
    std::expected<void, McpError> write(std::string_view message);
    std::expected<std::string, McpError> read();
    RequestMeta meta() const;
    std::int64_t next_id() noexcept;

    std::istream* m_input;
    std::ostream* m_output;
    ClientConfig m_config;
    std::atomic<std::int64_t> m_nextId{0};
    std::atomic_flag m_requestActive{};
};

/**
 * @brief HTTP client owned by runtime.get_io_scheduler(0).
 * @details Start runtime before construction and keep it alive through all calls.
 *          Submit calls to owner(); foreign schedulers return InvalidParams.
 *          Overlapping requests return Overload. Lifecycle tasks must be created
 *          and awaited on owner(), to completion before any other operation.
 *          listen() owns a separate connection but runs on the same owner.
 *          Destroy only after all calls finish. Tool definitions are owned values.
 */
class McpHttpClient {
public:
    using SubscriptionCallback = std::function<bool(std::string)>;
    using ConnectAwaitable = decltype(std::declval<http::HttpClient&>().connect(
        std::declval<const std::string&>()));
    using CloseAwaitable = decltype(std::declval<http::HttpClient&>().close());

    McpHttpClient(kernel::Runtime& runtime, std::string url, ClientConfig config = {});
    McpHttpClient(const McpHttpClient&) = delete;
    McpHttpClient& operator=(const McpHttpClient&) = delete;
    McpHttpClient(McpHttpClient&&) = delete;
    McpHttpClient& operator=(McpHttpClient&&) = delete;
    kernel::Scheduler* owner() const noexcept { return m_owner; }
    /** @brief Check owner and return the existing transport task without wrapping it. */
    std::expected<ConnectAwaitable, McpError> connect();
    /** @brief Check owner and return the existing transport task without wrapping it. */
    std::expected<CloseAwaitable, McpError> close();

    kernel::Task<void> discover(std::expected<DiscoverResult, McpError>& result);
    kernel::Task<void> list_tools(std::expected<std::vector<Tool>, McpError>& result);
    kernel::Task<void> call_tool(std::string name, std::string arguments,
                                std::expected<std::string, McpError>& result);
    kernel::Task<void> list_resources(std::expected<std::vector<Resource>, McpError>& result);
    kernel::Task<void> read_resource(std::string uri,
                                    std::expected<std::string, McpError>& result);
    kernel::Task<void> list_prompts(std::expected<std::vector<Prompt>, McpError>& result);
    kernel::Task<void> get_prompt(std::string name, std::string arguments,
                                 std::expected<std::string, McpError>& result);
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
                               std::expected<std::string, McpError>& result);
    RequestMeta meta() const;
    std::int64_t next_id() noexcept;
    bool on_owner() const noexcept;

    http::HttpClient m_client;
    ClientConfig m_config;
    using ToolDefinitions = std::unordered_map<std::string, Tool>;
    ToolDefinitions m_toolDefinitions;
    std::string m_url;
    kernel::Scheduler* const m_owner;
    std::int64_t m_nextId{0};
    bool m_requestActive{false};
};

} // namespace galay::mcp::v2

#endif // GALAY_MCP_V2_CLIENT_H
