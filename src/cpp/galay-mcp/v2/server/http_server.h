/**
 * @file http_server.h
 * @brief MCP 2026-07-28 Streamable HTTP server.
 */

#ifndef GALAY_MCP_V2_HTTP_SERVER_H
#define GALAY_MCP_V2_HTTP_SERVER_H

#include "../common/protocol.h"
#include "../../common/mcp_policy.h"
#include "../../../galay-http/server/http_server.h"
#include "../../../galay-http/server/http_router.h"
#include "../../../galay-kernel/concurrency/mpmc/bounded_channel.h"

#include <atomic>
#include <functional>
#include <map>
#include <memory>

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
    /** @brief 注册阶段接口；必须在 start() 前由单线程调用。 */
    void addTool(std::string name, std::string description, JsonString inputSchema,
                 ToolHandler handler);
    /** @brief 注册阶段接口；必须在 start() 前由单线程调用。 */
    void addResource(std::string uri, std::string name, std::string description,
                     std::string mimeType, ResourceReader reader);
    /** @brief 注册阶段接口；必须在 start() 前由单线程调用。 */
    void addPrompt(std::string name, std::string description,
                   std::vector<PromptArgument> arguments, PromptGetter getter);
    /** @return 成功入队的工具列表变更通知数。 */
    std::size_t notifyToolsListChanged();
    /** @return 成功入队的资源列表变更通知数。 */
    std::size_t notifyResourcesListChanged();
    /** @return 成功入队的提示列表变更通知数。 */
    std::size_t notifyPromptsListChanged();
    /** @return 成功入队的指定资源变更通知数。 */
    std::size_t notifyResourceUpdated(std::string_view uri);
    void start();
    void stop();
    bool isRunning() const noexcept;

private:
    struct ToolEntry { Tool tool; ToolHandler handler; };
    struct ResourceEntry { Resource resource; ResourceReader reader; };
    struct PromptEntry { Prompt prompt; PromptGetter getter; };
    struct HttpResult { int status = 200; JsonString body; };
    enum class LifecycleState : unsigned char {
        kStopped,
        kStarting,
        kRunning,
        kStopping,
    };
    struct Subscription {
        explicit Subscription(RequestId requestId, SubscriptionFilter acceptedFilter)
            : id(std::move(requestId))
            , filter(std::move(acceptedFilter))
            , events(256)
        {
        }

        RequestId id;
        SubscriptionFilter filter;
        galay::mpmc::BoundedChannel<JsonString> events;
    };
    using SubscriptionSnapshot =
        std::map<std::uint64_t, std::shared_ptr<Subscription>>;

    galay::kernel::Task<void> process(http::HttpConn& conn, http::HttpRequest& request);
    galay::kernel::Task<void> listen(http::HttpConn& conn,
                                     const ParsedRequest& request);
    galay::kernel::Task<void> sendResponse(http::HttpConn& conn, const HttpResult& result);
    galay::kernel::Task<HttpResult> dispatch(const ParsedRequest& request);
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
    SubscriptionFilter acceptedFilter(const SubscriptionFilter& requested) const;
    std::size_t publish(std::string_view method,
                        const std::function<bool(const SubscriptionFilter&)>& matches,
                        std::optional<std::string_view> uri = std::nullopt);
    std::optional<std::uint64_t> registerSubscription(
        const std::shared_ptr<Subscription>& subscription);
    void eraseSubscription(std::uint64_t token);
    void closeSubscriptions();
    void finishSubscription() noexcept;

    std::string m_host;
    std::string m_serverName{"galay-mcp-v2-http"};
    std::string m_serverVersion{"2.0.0"};
    std::size_t m_ioSchedulers;
    std::size_t m_computeSchedulers;
    int m_port;
    bool m_tcpNoDelay;
    McpProductionPolicy m_policy;
    std::map<std::string, ToolEntry> m_tools;             ///< 注册阶段写入，运行期只读
    std::map<std::string, ResourceEntry> m_resources;     ///< 注册阶段写入，运行期只读
    std::map<std::string, PromptEntry> m_prompts;         ///< 注册阶段写入，运行期只读
    std::atomic<std::shared_ptr<http::HttpServer>> m_httpServer;
    std::atomic<std::shared_ptr<const SubscriptionSnapshot>> m_subscriptions{
        std::make_shared<const SubscriptionSnapshot>()};
    std::atomic<std::uint64_t> m_nextSubscriptionToken{0};
    std::atomic<std::size_t> m_activeSubscriptions{0};
    std::atomic<LifecycleState> m_lifecycle{LifecycleState::kStopped};
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_hasTools{false};
    std::atomic<bool> m_hasResources{false};
    std::atomic<bool> m_hasPrompts{false};
};

} // namespace galay::mcp::v2

#endif // GALAY_MCP_V2_HTTP_SERVER_H
