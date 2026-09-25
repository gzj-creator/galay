#include "http_server.h"
#include "../common/http_headers.h"

#include <charconv>
#include <chrono>
#include <algorithm>
#include <string>
#include <thread>
#include <new>

namespace galay::mcp::v2 {

namespace {
std::expected<json::Json, McpError> objectParams(const ParsedRequest& request)
{
    if (!request.request.params.is_object()) {
        return std::unexpected(McpError::invalidParams("params must be an object"));
    }
    return request.request.params;
}

std::expected<std::string, McpError> required(const json::Json& object, const char* key)
{
    auto value = object.at(key).as_string();
    if (!value) {
        return std::unexpected(McpError::invalidParams(
            std::string("missing or invalid ") + key));
    }
    return std::string(*value);
}

std::expected<json::Json, McpError> optionalObject(const json::Json& object,
                                                    const char* key)
{
    const json::Json value = object.at(key);
    if (!value.valid()) {
        return galay::mcp::emptyJsonObject();
    }
    if (!value.is_object()) {
        return std::unexpected(McpError::invalidParams(
            std::string(key) + " must be an object"));
    }
    return value;
}

bool headerValueMatches(std::string_view expected,
                        std::string_view actual,
                        std::string_view type)
{
    if (type != "integer") return expected == actual;
    int64_t expectedValue = 0;
    const auto expectedResult = std::from_chars(
        expected.data(), expected.data() + expected.size(), expectedValue);
    if (expectedResult.ec != std::errc{} ||
        expectedResult.ptr != expected.data() + expected.size()) {
        return false;
    }
    int64_t actualValue = 0;
    const auto actualResult = std::from_chars(
        actual.data(), actual.data() + actual.size(), actualValue);
    return actualResult.ec == std::errc{} &&
           actualResult.ptr == actual.data() + actual.size() &&
           actualValue == expectedValue;
}

std::string promptResult(std::string_view value)
{
    auto parsed = parseResult(value);
    if (parsed) {
        return std::string(value);
    }
    auto document = JsonDocument::parse(value);
    if (!document) {
        return R"({"resultType":"complete","messages":[]})";
    }
    if (!document->root().is_object()) {
        return R"({"resultType":"complete","messages":[]})";
    }
    const json::Json& object = document->root();
    std::string out;
    auto writer = makeJsonWriter(out);
    // StreamWriter 失败粘滞：中间结果统一丢弃，由 finish() 统一检查
    std::string raw;
    (void)writer.start_object();
    (void)writer.key("resultType");
    (void)writer.string("complete");
    object.for_each_member([&](std::string_view fieldKey, const json::Json& member) -> json::result<void> {
        if (fieldKey == "resultType") {
            return {};
        }
        raw.clear();
        auto serialized = json::stream::serialize(
            member, [&](std::string_view chunk) -> json::result<void> {
                raw.append(chunk);
                return {};
            });
        if (!serialized) {
            return {};
        }
        (void)writer.key(fieldKey);
        (void)writer.raw(raw);
        return {};
    });
    (void)writer.end_object();
    if (!writer.finish()) {
        return R"({"resultType":"complete","messages":[]})";
    }
    return out;
}

} // namespace

McpHttpServer::McpHttpServer(std::string host,
                             int port,
                             std::size_t ioSchedulers,
                             std::size_t parallelSchedulers,
                             bool tcpNoDelay)
    : m_httpServer(http::HttpServerBuilder().host(std::move(host))
                       .port(static_cast<uint16_t>(port))
                       .backlog(128).ioSchedulerCount(ioSchedulers)
                       .parallelSchedulerCount(parallelSchedulers)
                       .tcpNoDelay(tcpNoDelay).buildConfig())
{
}

McpHttpServer::~McpHttpServer() { stop(); }

void McpHttpServer::setServerInfo(std::string name, std::string version)
{
    m_serverName = std::move(name);
    m_serverVersion = std::move(version);
}

void McpHttpServer::setProductionPolicy(McpProductionPolicy policy)
{
    m_policy = std::move(policy);
}

void McpHttpServer::addTool(std::string name,
                            std::string description,
                            std::string inputSchema,
                            ToolHandler handler)
{
    ToolEntry entry;
    entry.tool.name = std::move(name);
    entry.tool.description = std::move(description);
    entry.tool.inputSchema = std::move(inputSchema);
    entry.handler = std::move(handler);
    m_tools.insert_or_assign(entry.tool.name, std::move(entry));
}

void McpHttpServer::addResource(std::string uri,
                                std::string name,
                                std::string description,
                                std::string mimeType,
                                ResourceReader reader)
{
    ResourceEntry entry;
    entry.resource.uri = std::move(uri);
    entry.resource.name = std::move(name);
    entry.resource.description = std::move(description);
    entry.resource.mimeType = std::move(mimeType);
    entry.reader = std::move(reader);
    m_resources.insert_or_assign(entry.resource.uri, std::move(entry));
}

void McpHttpServer::addPrompt(std::string name,
                              std::string description,
                              std::vector<PromptArgument> arguments,
                              PromptGetter getter)
{
    PromptEntry entry;
    entry.prompt.name = std::move(name);
    entry.prompt.description = std::move(description);
    entry.prompt.arguments = std::move(arguments);
    entry.getter = std::move(getter);
    m_prompts.insert_or_assign(entry.prompt.name, std::move(entry));
}

McpHttpServer::Operation McpHttpServer::Operation::acquire(
    std::atomic<std::size_t>& count) noexcept
{
    auto active = count.load(std::memory_order_acquire);
    while ((active & kAdmissionClosed) == 0) {
        if (count.compare_exchange_weak(active, active + 1,
                                        std::memory_order_acq_rel,
                                        std::memory_order_acquire)) {
            return Operation{&count};
        }
    }
    return Operation{nullptr};
}

McpHttpServer::Operation::~Operation()
{
    if (m_count != nullptr) {
        m_count->fetch_sub(1, std::memory_order_release);
        m_count->notify_all();
    }
}

std::expected<void, McpError> McpHttpServer::notifyToolsListChanged()
{
    return submit(Command{CommandKind::Tools});
}

std::expected<void, McpError> McpHttpServer::notifyResourcesListChanged()
{
    return submit(Command{CommandKind::Resources});
}

std::expected<void, McpError> McpHttpServer::notifyPromptsListChanged()
{
    return submit(Command{CommandKind::Prompts});
}

std::expected<void, McpError> McpHttpServer::notifyResourceUpdated(std::string uri)
{
    return submit(Command{CommandKind::Resource, std::move(uri)});
}

std::expected<void, McpError> McpHttpServer::submit(Command command)
{
    auto operation = Operation::acquire(m_admission);
    if (!operation) return std::unexpected(McpError::connectionClosed("HTTP server is not accepting commands"));
    if (!m_commands.send(std::move(command))) {
        return std::unexpected(McpError::overload("HTTP owner command queue allocation failed"));
    }
    wakeOwner();
    return {};
}

void McpHttpServer::wakeOwner() noexcept
{
    m_wakeSequence.fetch_add(1, std::memory_order_release);
    m_wakeSequence.notify_one();
}

bool McpHttpServer::processCommands()
{
    // Bound each pass so a busy producer cannot starve shutdown or release.
    std::size_t processed = 0;
    while (processed != 256) {
        auto command = m_commands.tryRecv();
        if (!command) break;
        ++processed;
        if (command->kind == CommandKind::Register) {
            auto* subscription = command->subscription.get();
            subscription->next = std::move(m_subscriptions);
            m_subscriptions = std::move(command->subscription);
            if (!subscription->registered.notify(true)) {
                subscription->events.close();
            }
        } else {
            publish(command->kind, command->uri);
        }
    }
    return processed != 0;
}

void McpHttpServer::publish(CommandKind notification, const std::string& uri)
{
    for (auto* subscription = m_subscriptions.get(); subscription; subscription = subscription->next.get()) {
        if (subscription->finished.load(std::memory_order_acquire)) continue;
        const auto& filter = subscription->filter;
        std::string_view method;
        switch (notification) {
        case CommandKind::Register:
            return;
        case CommandKind::Tools:
            if (!filter.toolsListChanged) continue;
            method = NotificationMethods::TOOLS_LIST_CHANGED;
            break;
        case CommandKind::Resources:
            if (!filter.resourcesListChanged) continue;
            method = NotificationMethods::RESOURCES_LIST_CHANGED;
            break;
        case CommandKind::Prompts:
            if (!filter.promptsListChanged) continue;
            method = NotificationMethods::PROMPTS_LIST_CHANGED;
            break;
        case CommandKind::Resource:
            if (std::find(filter.resourceSubscriptions.begin(),
                          filter.resourceSubscriptions.end(), uri) ==
                filter.resourceSubscriptions.end()) continue;
            method = NotificationMethods::RESOURCES_UPDATED;
            break;
        }
        auto message = makeSubscriptionNotification(
            method, subscription->id, notification == CommandKind::Resource
                ? std::optional<std::string_view>(uri) : std::nullopt);
        // Preserve the bounded subscriber queue: overload drops this event.
        if (!subscription->events.trySend(std::move(message))) continue;
    }
}

McpHttpServer::HttpResult McpHttpServer::error(const std::optional<RequestId>& id,
                                               int code,
                                               std::string_view message,
                                               std::optional<std::string_view> data,
                                               int status) const
{
    return HttpResult{status, makeErrorResponse(id, code, message, data)};
}

McpHttpServer::HttpResult McpHttpServer::error(const std::optional<RequestId>& id,
                                               const McpError& value,
                                               int status) const
{
    return error(id,
                 value.toJsonRpcErrorCode(),
                 value.message(),
                 value.details().empty()
                     ? std::nullopt
                     : std::optional<std::string_view>(value.details()),
                 status);
}

std::expected<std::string, McpError> McpHttpServer::headerName(
    http::HttpRequest& request,
    const ParsedRequest& parsed) const
{
    const std::string method = request.header().headerPairs().getValue("Mcp-Method");
    if (method.empty() || method != parsed.request.method) {
        return std::unexpected(McpError::protocolError("Mcp-Method header mismatch"));
    }
    if (parsed.request.method != Methods::TOOLS_CALL &&
        parsed.request.method != Methods::RESOURCES_READ &&
        parsed.request.method != Methods::PROMPTS_GET) {
        return std::string{};
    }
    auto name = decodeHeaderValue(request.header().headerPairs().getValue("Mcp-Name"));
    if (!name || name->empty()) {
        return std::unexpected(McpError::protocolError("missing Mcp-Name header"));
    }
    return name.value();
}

std::expected<void, McpError> McpHttpServer::validateHeaders(
    http::HttpRequest& request,
    const ParsedRequest& parsed) const
{
    const auto& headers = request.header().headerPairs();
    const std::string accept = headers.getValue("Accept");
    if (accept.find("application/json") == std::string::npos ||
        accept.find("text/event-stream") == std::string::npos) {
        return std::unexpected(McpError::protocolError("Accept must include JSON and SSE"));
    }
    const std::string version = headers.getValue("MCP-Protocol-Version");
    if (version.empty() || version != parsed.request.meta.protocolVersion) {
        return std::unexpected(McpError::protocolError("MCP-Protocol-Version header mismatch"));
    }
    auto name = headerName(request, parsed);
    if (!name) {
        return std::unexpected(name.error());
    }
    if (!name->empty()) {
        auto params = objectParams(parsed);
        if (!params) {
            return std::unexpected(params.error());
        }
        const char* key = parsed.request.method == Methods::RESOURCES_READ ? "uri" : "name";
        auto bodyName = required(params.value(), key);
        if (!bodyName || bodyName.value() != name.value()) {
            return std::unexpected(McpError::protocolError("Mcp-Name header mismatch"));
        }
        if (parsed.request.method == Methods::TOOLS_CALL) {
            json::Json arguments = params.value().at("arguments");
            if (!arguments.valid()) {
                arguments = galay::mcp::emptyJsonObject();
            }
            Tool tool;
            auto it = m_tools.find(name.value());
            if (it == m_tools.end()) return {};
            tool = it->second.tool;
            auto annotations = toolHeaderAnnotations(tool);
            if (!annotations) return std::unexpected(annotations.error());
            const auto& headerPairs = request.header().headerPairs();
            for (const auto& annotation : annotations.value()) {
                auto bodyValue = argumentHeaderValue(arguments, annotation);
                if (!bodyValue) return std::unexpected(bodyValue.error());
                const std::string headerName = "Mcp-Param-" + annotation.name;
                const bool hasHeader = headerPairs.hasKey(headerName);
                if (!bodyValue.value()) {
                    if (hasHeader) {
                        return std::unexpected(McpError::protocolError(
                            "unexpected " + headerName + " header"));
                    }
                    continue;
                }
                if (!hasHeader) {
                    return std::unexpected(McpError::protocolError(
                        "missing " + headerName + " header"));
                }
                auto headerValue = decodeHeaderValue(headerPairs.getValue(headerName));
                if (!headerValue || !headerValueMatches(*bodyValue.value(),
                                                         headerValue.value(),
                                                         annotation.type)) {
                    return std::unexpected(McpError::protocolError(
                        headerName + " header mismatch"));
                }
            }
        }
    }
    return {};
}

galay::kernel::Task<McpHttpServer::HttpResult> McpHttpServer::dispatch(
    const ParsedRequest& parsed)
{
    if (parsed.request.meta.protocolVersion != MCP_VERSION) {
        co_return HttpResult{400, makeUnsupportedProtocolVersionResponse(
            parsed.request.id, parsed.request.meta.protocolVersion, {MCP_VERSION})};
    }
    auto params = objectParams(parsed);
    if (!params) {
        co_return error(parsed.request.id, params.error());
    }
    const RequestId& id = parsed.request.id;
    const json::Json object = params.value();

    if (parsed.request.method == Methods::SERVER_DISCOVER) {
        DiscoverResult result;
        result.serverInfo = Implementation{.name = m_serverName, .version = m_serverVersion};
        result.capabilities.tools = !m_tools.empty();
        result.capabilities.resources = !m_resources.empty();
        result.capabilities.prompts = !m_prompts.empty();
        result.capabilities.toolsListChanged = !m_tools.empty();
        result.capabilities.resourcesListChanged = !m_resources.empty();
        result.capabilities.resourceSubscriptions = !m_resources.empty();
        result.capabilities.promptsListChanged = !m_prompts.empty();
        co_return HttpResult{200, makeResultResponse(id, result.toJson())};
    }

    if (parsed.request.method == Methods::TOOLS_LIST ||
        parsed.request.method == Methods::RESOURCES_LIST ||
        parsed.request.method == Methods::PROMPTS_LIST) {
        ListResult result;
        result.field = parsed.request.method == Methods::TOOLS_LIST
            ? "tools" : parsed.request.method == Methods::RESOURCES_LIST ? "resources" : "prompts";
        if (result.field == "tools") {
            for (const auto& [unused, entry] : m_tools) result.items.push_back(entry.tool.toJson());
        } else if (result.field == "resources") {
            for (const auto& [unused, entry] : m_resources) result.items.push_back(entry.resource.toJson());
        } else {
            for (const auto& [unused, entry] : m_prompts) result.items.push_back(entry.prompt.toJson());
        }
        co_return HttpResult{200, makeResultResponse(id, result.toJson())};
    }

    if (parsed.request.method == Methods::TOOLS_CALL) {
        auto name = required(object, "name");
        if (!name) { co_return error(id, name.error()); }
        auto arguments = optionalObject(object, "arguments");
        if (!arguments) { co_return error(id, arguments.error()); }
        ToolHandler handler;
        auto it = m_tools.find(name.value());
        if (it == m_tools.end()) co_return error(id, ErrorCodes::METHOD_NOT_FOUND, "Method not found", name.value(), 404);
        handler = it->second.handler;
        std::expected<std::string, McpError> value;
        co_await handler(arguments.value(), value);
        if (!value) co_return error(id, value.error());
        co_return HttpResult{200, makeResultResponse(id, ToolCallResult::text(value.value()).toJson())};
    }

    if (parsed.request.method == Methods::RESOURCES_READ) {
        auto uri = required(object, "uri");
        if (!uri) { co_return error(id, uri.error()); }
        ResourceReader reader;
        std::optional<std::string> mimeType;
        auto it = m_resources.find(uri.value());
        if (it == m_resources.end()) co_return error(id, ErrorCodes::INVALID_PARAMS, "Resource not found", uri.value());
        reader = it->second.reader;
        mimeType = it->second.resource.mimeType;
        std::expected<std::string, McpError> value;
        co_await reader(uri.value(), value);
        if (!value) co_return error(id, value.error());
        co_return HttpResult{200, makeResultResponse(id, ReadResourceResult::text(uri.value(), value.value(), mimeType).toJson())};
    }

    if (parsed.request.method == Methods::PROMPTS_GET) {
        auto name = required(object, "name");
        if (!name) { co_return error(id, name.error()); }
        auto arguments = optionalObject(object, "arguments");
        if (!arguments) { co_return error(id, arguments.error()); }
        PromptGetter getter;
        auto it = m_prompts.find(name.value());
        if (it == m_prompts.end()) co_return error(id, ErrorCodes::METHOD_NOT_FOUND, "Method not found", name.value(), 404);
        getter = it->second.getter;
        std::expected<std::string, McpError> value;
        co_await getter(name.value(), arguments.value(), value);
        if (!value) co_return error(id, value.error());
        co_return HttpResult{200, makeResultResponse(id, promptResult(value.value()))};
    }

    co_return error(id, ErrorCodes::METHOD_NOT_FOUND, "Method not found", parsed.request.method, 404);
}

bool McpHttpServer::validOrigin(http::HttpRequest& request) const
{
    const std::string origin = request.header().headerPairs().getValue("Origin");
    if (origin.empty()) {
        return true;
    }
    return origin == "http://localhost" || origin == "http://127.0.0.1" ||
           origin == "https://localhost" || origin == "https://127.0.0.1";
}

SubscriptionFilter McpHttpServer::acceptedFilter(
    const SubscriptionFilter& requested) const
{
    SubscriptionFilter accepted;
    accepted.toolsListChanged =
        requested.toolsListChanged && !m_tools.empty();
    accepted.promptsListChanged =
        requested.promptsListChanged && !m_prompts.empty();
    accepted.resourcesListChanged =
        requested.resourcesListChanged && !m_resources.empty();
    if (!m_resources.empty()) {
        for (const auto& uri : requested.resourceSubscriptions) {
            if (m_resources.contains(uri)) {
                accepted.resourceSubscriptions.push_back(uri);
            }
        }
    }
    return accepted;
}

void McpHttpServer::reapSubscriptions()
{
    auto* link = &m_subscriptions;
    while (auto* subscription = link->get()) {
        if (!subscription->finished.load(std::memory_order_acquire)) {
            link = &subscription->next;
            continue;
        }
        auto finished = std::move(*link);
        *link = std::move(finished->next);
    }
}

void McpHttpServer::closeSubscriptions()
{
    for (auto* subscription = m_subscriptions.get(); subscription; subscription = subscription->next.get()) {
        subscription->events.close();
    }
}

galay::kernel::Task<void> McpHttpServer::listen(
    http::HttpConn& conn,
    const ParsedRequest& request)
{
    auto params = objectParams(request);
    if (!params) {
        co_await sendResponse(conn, error(request.request.id, params.error()));
        co_return;
    }
    const json::Json notifications = params.value().at("notifications");
    if (!notifications.valid()) {
        co_await sendResponse(
            conn, error(request.request.id,
                        McpError::invalidParams("missing notifications")));
        co_return;
    }
    auto requested = SubscriptionFilter::fromJson(notifications);
    if (!requested) {
        co_await sendResponse(conn, error(request.request.id, requested.error()));
        co_return;
    }

    auto operation = Operation::acquire(m_activeSubscriptions);
    if (!operation) co_return;
    // The channel requires cache-line alignment, beyond the Task frame contract.
    // Transfer aligned storage to the owner; the listener only borrows it.
    auto state = std::unique_ptr<Subscription>(new (std::nothrow) Subscription{
        request.request.id, acceptedFilter(requested.value())});
    if (!state) {
        co_await sendResponse(conn, error(request.request.id,
            McpError::overload("subscription allocation failed")));
        co_return;
    }
    auto* subscription = state.get();
    auto submitted = submit(Command{CommandKind::Register, {}, std::move(state)});
    if (!submitted) {
        co_await sendResponse(conn, error(request.request.id, submitted.error()));
        co_return;
    }
    const auto registered = co_await subscription->registered.wait();
    // No timeout: the owner retains the node through registration and listening.
    if (!registered || !*registered) {
        subscription->finished.store(true, std::memory_order_release);
        wakeOwner();
        co_return;
    }

    http::HttpResponseHeader header;
    header.version() = http::HttpVersion::HttpVersion_1_1;
    header.code() = http::HttpStatusCode::OK_200;
    header.headerPairs().addHeaderPair("Content-Type", "text/event-stream");
    header.headerPairs().addHeaderPair("Cache-Control", "no-cache");
    header.headerPairs().addHeaderPair("Transfer-Encoding", "chunked");
    header.headerPairs().addHeaderPair("Connection", "close");
    header.headerPairs().addHeaderPair("X-Accel-Buffering", "no");
    auto writer = conn.getWriter();

    do {
        auto sendHeader = co_await writer.sendHeader(std::move(header));
        if (!sendHeader || !sendHeader.value()) break;
        const auto acknowledged = encodeSseEvent(
            makeSubscriptionAcknowledgedNotification(subscription->id, subscription->filter));
        auto sendAcknowledged = co_await writer.sendChunk(acknowledged);
        if (!sendAcknowledged || !sendAcknowledged.value()) break;

        while (true) {
            auto event = co_await subscription->events.recv().timeout(
                std::chrono::seconds(15));
            if (!event) {
                if (galay::kernel::IOError::contains(
                        event.error().code(), galay::kernel::kTimeout)) {
                    auto keepAlive = co_await writer.sendChunk(": keep-alive\n\n");
                    if (keepAlive && keepAlive.value()) continue;
                }
                break;
            }
            auto encoded = encodeSseEvent(event.value());
            auto sent = co_await writer.sendChunk(encoded);
            if (!sent || !sent.value()) {
                subscription->events.close();
                break;
            }
        }

        if (m_running.load(std::memory_order_acquire)) break;
        auto complete = encodeSseEvent(makeSubscriptionCompleteResponse(subscription->id));
        auto completeSent = co_await writer.sendChunk(complete);
        if (completeSent && completeSent.value()) {
            auto finalSent = co_await writer.sendChunk(std::string{}, true);
            if (!finalSent || !*finalSent) subscription->events.close();
        }
    } while (false);
    // Last node access: the owner may reclaim it as soon as finished is visible.
    // The operation lease keeps the server alive through wakeOwner().
    subscription->finished.store(true, std::memory_order_release);
    wakeOwner();
}

galay::kernel::Task<void> McpHttpServer::sendResponse(http::HttpConn& conn,
                                                      const HttpResult& result)
{
    const std::string body = result.body;
    std::string wire;
    wire.reserve(body.size() + 180);
    wire += "HTTP/1.1 ";
    wire += std::to_string(result.status);
    wire += result.status == 200 ? " OK\r\n" :
        result.status == 400 ? " Bad Request\r\n" :
        result.status == 403 ? " Forbidden\r\n" : " Not Found\r\n";
    wire += "Server: " + m_serverName + "/" + m_serverVersion + "\r\n";
    wire += "Content-Type: application/json\r\nConnection: keep-alive\r\nContent-Length: ";
    wire += std::to_string(body.size());
    wire += "\r\n\r\n";
    wire += body;
    auto writer = conn.getWriter();
    while (true) {
        auto sent = co_await writer.send(std::move(wire));
        if (!sent || sent.value()) break;
    }
    co_return;
}

galay::kernel::Task<void> McpHttpServer::process(http::HttpConn& conn,
                                                 http::HttpRequest& request)
{
    if (!validOrigin(request)) {
        co_await sendResponse(conn, error(std::nullopt, ErrorCodes::INVALID_REQUEST,
                                         "Invalid Origin", std::nullopt, 403));
        co_return;
    }
    auto parsed = parseRequest(request.bodyStr());
    if (!parsed) {
        co_await sendResponse(conn, error(std::nullopt, parsed.error()));
        co_return;
    }
    auto headerValidation = validateHeaders(request, parsed.value());
    if (!headerValidation) {
        co_await sendResponse(conn, error(parsed->request.id,
                                         ErrorCodes::HEADER_MISMATCH,
                                         "Header mismatch",
                                         headerValidation.error().details(),
                                         400));
        co_return;
    }
    if (parsed->request.meta.protocolVersion != MCP_VERSION) {
        co_await sendResponse(conn, HttpResult{
            400, makeUnsupportedProtocolVersionResponse(
                     parsed->request.id,
                     parsed->request.meta.protocolVersion,
                     {MCP_VERSION})});
        co_return;
    }
    if (parsed->request.method == Methods::SUBSCRIPTIONS_LISTEN) {
        co_await listen(conn, parsed.value());
        co_return;
    }
    auto dispatched = co_await dispatch(parsed.value());
    HttpResult result;
    if (!dispatched) {
        result = error(std::nullopt, ErrorCodes::INTERNAL_ERROR, "Internal error", std::nullopt, 500);
    } else {
        result = std::move(dispatched.value());
    }
    if (result.body.size() > m_policy.transport.max_response_bytes) {
        result = error(std::nullopt, ErrorCodes::INVALID_REQUEST, "Payload too large", std::nullopt, 400);
    }
    co_await sendResponse(conn, result);
}

void McpHttpServer::start()
{
    auto expected = LifecycleState::kStopped;
    if (!m_lifecycle.compare_exchange_strong(
            expected, LifecycleState::kStarting,
            std::memory_order_acq_rel, std::memory_order_acquire)) {
        return;
    }
    http::HttpRouter router;
    auto* server = this;
    router.addHandler<http::HttpMethod::POST>("/mcp",
        [server](http::HttpConn& conn, http::HttpRequest request) -> galay::kernel::Task<void> {
            if (request.bodyStr().size() > server->m_policy.transport.max_http_body_bytes) {
                co_await server->sendResponse(conn, server->error(std::nullopt,
                    ErrorCodes::INVALID_REQUEST, "Payload too large", std::nullopt, 400));
                co_return;
            }
            co_await server->process(conn, request);
        });
    m_httpServer.start(std::move(router));
    if (!m_httpServer.isRunning()) {
        m_httpServer.stop();
        m_lifecycle.store(LifecycleState::kStopped, std::memory_order_release);
        m_lifecycle.notify_all();
        return;
    }
    m_activeSubscriptions.store(0, std::memory_order_release);
    m_admission.store(0, std::memory_order_release);
    m_running.store(true, std::memory_order_release);
    m_lifecycle.store(LifecycleState::kRunning, std::memory_order_release);
    m_lifecycle.notify_all();
    while (m_lifecycle.load(std::memory_order_acquire) == LifecycleState::kRunning) {
        const auto sequence = m_wakeSequence.load(std::memory_order_acquire);
        const bool processed = processCommands();
        reapSubscriptions();
        if (!processed && m_lifecycle.load(std::memory_order_acquire) == LifecycleState::kRunning) {
            m_wakeSequence.wait(sequence, std::memory_order_acquire);
        }
    }
    m_running.store(false, std::memory_order_release);
    // Close admission before waiting: no new operation can extend the drain.
    // Previous counts are irrelevant; the closed bit excludes new borrowers.
    (void)m_activeSubscriptions.fetch_or(kAdmissionClosed, std::memory_order_acq_rel);
    (void)m_admission.fetch_or(kAdmissionClosed, std::memory_order_acq_rel);
    auto active = m_admission.load(std::memory_order_acquire);
    while (active != kAdmissionClosed) {
        m_admission.wait(active, std::memory_order_acquire);
        active = m_admission.load(std::memory_order_acquire);
    }
    while (processCommands()) { reapSubscriptions(); }
    closeSubscriptions();
    while (m_subscriptions != nullptr) {
        const auto sequence = m_wakeSequence.load(std::memory_order_acquire);
        reapSubscriptions();
        if (m_subscriptions != nullptr) m_wakeSequence.wait(sequence, std::memory_order_acquire);
    }
    active = m_activeSubscriptions.load(std::memory_order_acquire);
    while (active != kAdmissionClosed) {
        m_activeSubscriptions.wait(active, std::memory_order_acquire);
        active = m_activeSubscriptions.load(std::memory_order_acquire);
    }
    m_httpServer.stop();
    m_lifecycle.store(LifecycleState::kStopped, std::memory_order_release);
    m_lifecycle.notify_all();
}

void McpHttpServer::stop()
{
    auto state = m_lifecycle.load(std::memory_order_acquire);
    while (state == LifecycleState::kStarting) {
        m_lifecycle.wait(state, std::memory_order_acquire);
        state = m_lifecycle.load(std::memory_order_acquire);
    }
    if (state == LifecycleState::kRunning &&
        m_lifecycle.compare_exchange_strong(state, LifecycleState::kStopping,
                                            std::memory_order_acq_rel,
                                            std::memory_order_acquire)) {
        m_lifecycle.notify_all();
        wakeOwner();
        state = LifecycleState::kStopping;
    }
    while (state == LifecycleState::kStopping) {
        m_lifecycle.wait(state, std::memory_order_acquire);
        state = m_lifecycle.load(std::memory_order_acquire);
    }
}

bool McpHttpServer::isRunning() const noexcept { return m_running.load(); }

} // namespace galay::mcp::v2
