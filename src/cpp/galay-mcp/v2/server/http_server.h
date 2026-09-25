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
#include "../../../galay-kernel/concurrency/mpsc/unbounded_channel.h"
#include "../../../galay-kernel/async/async_waiter.h"

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <utility>

namespace galay::mcp::v2 {

class McpHttpServer {
public:
    using ToolHandler = std::function<galay::kernel::Task<void>(
        const json::Json&, std::expected<std::string, McpError>&)>;
    using ResourceReader = std::function<galay::kernel::Task<void>(
        const std::string&, std::expected<std::string, McpError>&)>;
    using PromptGetter = std::function<galay::kernel::Task<void>(
        const std::string&, const json::Json&, std::expected<std::string, McpError>&)>;

    McpHttpServer(std::string host = "127.0.0.1", int port = 8080,
                  std::size_t ioSchedulers = 8, std::size_t parallelSchedulers = 0,
                  bool tcpNoDelay = true);
    ~McpHttpServer();
    McpHttpServer(const McpHttpServer&) = delete;
    McpHttpServer& operator=(const McpHttpServer&) = delete;

    /** @brief Configuration API; call from one thread before start(). */
    void setServerInfo(std::string name, std::string version);
    /** @brief Configuration API; call from one thread before start(). */
    void setProductionPolicy(McpProductionPolicy policy);
    /** @brief 注册阶段接口；必须在 start() 前由单线程调用。 */
    void addTool(std::string name, std::string description, std::string inputSchema,
                 ToolHandler handler);
    /** @brief 注册阶段接口；必须在 start() 前由单线程调用。 */
    void addResource(std::string uri, std::string name, std::string description,
                     std::string mimeType, ResourceReader reader);
    /** @brief 注册阶段接口；必须在 start() 前由单线程调用。 */
    void addPrompt(std::string name, std::string description,
                   std::vector<PromptArgument> arguments, PromptGetter getter);
    /**
     * @brief Submit a notification without blocking or creating a coroutine.
     * @details Success means the owner accepted the command, not a delivery count.
     *          Callable from any thread while the server lives. Stopped admission
     *          returns ConnectionClosed; queue allocation failure returns Overload.
     *          Slow subscribers retain the existing bounded event-queue policy.
     */
    std::expected<void, McpError> notifyToolsListChanged();
    /** @brief Submit a resource-list notification; same admission contract. */
    std::expected<void, McpError> notifyResourcesListChanged();
    /** @brief Submit a prompt-list notification; same admission contract. */
    std::expected<void, McpError> notifyPromptsListChanged();
    /** @brief Submit a resource notification, owning uri until the owner consumes it. */
    std::expected<void, McpError> notifyResourceUpdated(std::string uri);
    /** @brief Blocking lifecycle owner; call on an external thread and join before destruction. */
    void start();
    /** @brief Request shutdown and wait for start() to drain; external threads only. */
    void stop();
    bool isRunning() const noexcept;

private:
    friend struct McpHttpServerTestAccess;
    static constexpr std::size_t kAdmissionClosed = std::size_t{1} << (sizeof(std::size_t) * 8 - 1);
    /** Pins command admission only until enqueue succeeds or fails. */
    class Operation {
    public:
        static Operation acquire(std::atomic<std::size_t>& count) noexcept;
        Operation(Operation&& other) noexcept : m_count(std::exchange(other.m_count, nullptr)) {}
        ~Operation();
        explicit operator bool() const noexcept { return m_count != nullptr; }
    private:
        explicit Operation(std::atomic<std::size_t>* count) noexcept : m_count(count) {}
        Operation(const Operation&) = delete;
        Operation& operator=(const Operation&) = delete;
        std::atomic<std::size_t>* m_count;
    };
    struct ToolEntry { Tool tool; ToolHandler handler; };
    struct ResourceEntry { Resource resource; ResourceReader reader; };
    struct PromptEntry { Prompt prompt; PromptGetter getter; };
    struct HttpResult { int status = 200; std::string body; };
    enum class LifecycleState : unsigned char {
        kStopped,
        kStarting,
        kRunning,
        kStopping,
    };
    struct Subscription {
        explicit Subscription(RequestId requestId, SubscriptionFilter acceptedFilter)
            : events(256)
            , id(std::move(requestId))
            , filter(std::move(acceptedFilter))
        {
        }

        galay::mpmc::BoundedChannel<std::string> events;
        kernel::AsyncWaiter<bool> registered;
        RequestId id;
        SubscriptionFilter filter;
        std::unique_ptr<Subscription> next; // Owned only by the start() thread.
        std::atomic<bool> finished{false};
    };
    enum class CommandKind { Register, Tools, Resources, Prompts, Resource };
    struct Command {
    public:
        explicit Command(CommandKind value, std::string resource = {},
                         std::unique_ptr<Subscription> state = {})
            : uri(std::move(resource)), subscription(std::move(state)), kind(value) {}
        Command(Command&&) noexcept = default;
        Command& operator=(Command&&) noexcept = default;
        std::string uri;
        std::unique_ptr<Subscription> subscription;
        CommandKind kind;
    private:
        Command(const Command&) = delete;
        Command& operator=(const Command&) = delete;
    };

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
    std::expected<void, McpError> submit(Command command);
    void wakeOwner() noexcept;
    bool processCommands();
    void publish(CommandKind notification, const std::string& uri);
    void reapSubscriptions();
    void closeSubscriptions();

    mpsc::UnboundedChannel<Command> m_commands;
    http::HttpServer m_httpServer; // Constructed once; start() alone starts/stops it.
    McpProductionPolicy m_policy;
    std::map<std::string, ToolEntry> m_tools;
    std::map<std::string, ResourceEntry> m_resources;
    std::map<std::string, PromptEntry> m_prompts;
    std::string m_serverName{"galay-mcp-v2-http"};
    std::string m_serverVersion{"2.0.0"};
    std::unique_ptr<Subscription> m_subscriptions; // start() reclaims finished nodes.
    std::atomic<std::size_t> m_admission{kAdmissionClosed};
    std::atomic<std::size_t> m_activeSubscriptions{kAdmissionClosed};
    std::atomic<std::uint64_t> m_wakeSequence{0};
    std::atomic<LifecycleState> m_lifecycle{LifecycleState::kStopped};
    std::atomic<bool> m_running{false};
};

} // namespace galay::mcp::v2

#endif // GALAY_MCP_V2_HTTP_SERVER_H
