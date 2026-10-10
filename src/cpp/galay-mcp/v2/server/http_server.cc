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
std::expected<json::Json, McpError> object_params(const ParsedRequest& request)
{
    if (!request.request.params.is_object()) {
        return std::unexpected(McpError::invalid_params("params must be an object"));
    }
    return request.request.params;
}

std::expected<std::string, McpError> required(const json::Json& object, const char* key)
{
    auto value = json::decode_member<std::string>(object, key);
    if (!value) {
        return std::unexpected(McpError::invalid_params(
            std::string("missing or invalid ") + key));
    }
    return std::string(*value);
}

std::expected<json::Json, McpError> request_arguments(const json::Json& object)
{
    auto value = json::decode<ArgumentsFields>(object);
    if (!value) return std::unexpected(McpError::invalid_params(value.error()));
    return std::move(value->arguments);
}

bool header_value_matches(std::string_view expected,
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

std::string prompt_result(std::string_view fields) {
    auto value = json::merge_objects(fields, {{"resultType", {"\"complete\""}}});
    if (!value) return {};
    return std::move(*value);
}

} // namespace

McpHttpServer::McpHttpServer(std::string host,
                             int port,
                             std::size_t ioSchedulers,
                             std::size_t parallelSchedulers,
                             bool tcpNoDelay)
    : m_httpServer(http::HttpServerBuilder<>().host(std::move(host))
                       .port(static_cast<uint16_t>(port))
                       .backlog(128).io_scheduler_count(ioSchedulers)
                       .parallel_scheduler_count(parallelSchedulers)
                       .tcp_no_delay(tcpNoDelay).build_config())
{
}

McpHttpServer::~McpHttpServer() { stop(); }

void McpHttpServer::set_server_info(std::string name, std::string version)
{
    m_serverName = std::move(name);
    m_serverVersion = std::move(version);
}

void McpHttpServer::set_production_policy(McpProductionPolicy policy)
{
    m_policy = std::move(policy);
}

void McpHttpServer::add_tool(std::string name,
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

void McpHttpServer::add_resource(std::string uri,
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

void McpHttpServer::add_prompt(std::string name,
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

std::expected<void, McpError> McpHttpServer::notify_tools_list_changed()
{
    return submit(Command{CommandKind::Tools});
}

std::expected<void, McpError> McpHttpServer::notify_resources_list_changed()
{
    return submit(Command{CommandKind::Resources});
}

std::expected<void, McpError> McpHttpServer::notify_prompts_list_changed()
{
    return submit(Command{CommandKind::Prompts});
}

std::expected<void, McpError> McpHttpServer::notify_resource_updated(std::string uri)
{
    return submit(Command{CommandKind::Resource, std::move(uri)});
}

std::expected<void, McpError> McpHttpServer::submit(Command command)
{
    auto operation = Operation::acquire(m_admission);
    if (!operation) return std::unexpected(McpError::connection_closed("HTTP server is not accepting commands"));
    if (!m_commands.send(std::move(command))) {
        return std::unexpected(McpError::overload("HTTP owner command queue allocation failed"));
    }
    wake_owner();
    return {};
}

void McpHttpServer::wake_owner() noexcept
{
    m_wakeSequence.fetch_add(1, std::memory_order_release);
    m_wakeSequence.notify_one();
}

bool McpHttpServer::process_commands()
{
    // Bound each pass so a busy producer cannot starve shutdown or release.
    std::size_t processed = 0;
    while (processed != 256) {
        auto command = m_commands.try_recv();
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
        auto message = make_subscription_notification(
            method, subscription->id, notification == CommandKind::Resource
                ? std::optional<std::string_view>(uri) : std::nullopt);
        // Preserve the bounded subscriber queue: overload drops this event.
        if (!subscription->events.try_send(std::move(message))) continue;
    }
}

McpHttpServer::HttpResult McpHttpServer::error(const std::optional<RequestId>& id,
                                               int code,
                                               std::string_view message,
                                               std::optional<std::string_view> data,
                                               int status) const
{
    return HttpResult{status, make_error_response(id, code, message, data)};
}

McpHttpServer::HttpResult McpHttpServer::error(const std::optional<RequestId>& id,
                                               const McpError& value,
                                               int status) const
{
    return error(id,
                 value.to_json_rpc_error_code(),
                 value.message(),
                 value.details().empty()
                     ? std::nullopt
                     : std::optional<std::string_view>(value.details()),
                 status);
}

std::expected<std::string, McpError> McpHttpServer::header_name(
    http::HttpRequest& request,
    const ParsedRequest& parsed) const
{
    const std::string method = request.header().header_pairs().get_value("Mcp-Method");
    if (method.empty() || method != parsed.request.method) {
        return std::unexpected(McpError::protocol_error("Mcp-Method header mismatch"));
    }
    if (parsed.request.method != Methods::TOOLS_CALL &&
        parsed.request.method != Methods::RESOURCES_READ &&
        parsed.request.method != Methods::PROMPTS_GET) {
        return std::string{};
    }
    auto name = decode_header_value(request.header().header_pairs().get_value("Mcp-Name"));
    if (!name || name->empty()) {
        return std::unexpected(McpError::protocol_error("missing Mcp-Name header"));
    }
    return name.value();
}

std::expected<void, McpError> McpHttpServer::validate_headers(
    http::HttpRequest& request,
    const ParsedRequest& parsed) const
{
    const auto& headers = request.header().header_pairs();
    const std::string accept = headers.get_value("Accept");
    if (accept.find("application/json") == std::string::npos ||
        accept.find("text/event-stream") == std::string::npos) {
        return std::unexpected(McpError::protocol_error("Accept must include JSON and SSE"));
    }
    const std::string version = headers.get_value("MCP-Protocol-Version");
    if (version.empty() || version != parsed.request.meta.protocolVersion) {
        return std::unexpected(McpError::protocol_error("MCP-Protocol-Version header mismatch"));
    }
    auto name = header_name(request, parsed);
    if (!name) {
        return std::unexpected(name.error());
    }
    if (!name->empty()) {
        auto params = object_params(parsed);
        if (!params) {
            return std::unexpected(params.error());
        }
        const char* key = parsed.request.method == Methods::RESOURCES_READ ? "uri" : "name";
        auto bodyName = required(params.value(), key);
        if (!bodyName || bodyName.value() != name.value()) {
            return std::unexpected(McpError::protocol_error("Mcp-Name header mismatch"));
        }
        if (parsed.request.method == Methods::TOOLS_CALL) {
            auto arguments = request_arguments(params.value());
            if (!arguments) return std::unexpected(arguments.error());
            Tool tool;
            auto it = m_tools.find(name.value());
            if (it == m_tools.end()) return {};
            tool = it->second.tool;
            auto annotations = tool_header_annotations(tool);
            if (!annotations) return std::unexpected(annotations.error());
            const auto& header_pairs = request.header().header_pairs();
            for (const auto& annotation : annotations.value()) {
                auto bodyValue = argument_header_value(*arguments, annotation);
                if (!bodyValue) return std::unexpected(bodyValue.error());
                const std::string headerName = "Mcp-Param-" + annotation.name;
                const bool hasHeader = header_pairs.has_key(headerName);
                if (!bodyValue.value()) {
                    if (hasHeader) {
                        return std::unexpected(McpError::protocol_error(
                            "unexpected " + headerName + " header"));
                    }
                    continue;
                }
                if (!hasHeader) {
                    return std::unexpected(McpError::protocol_error(
                        "missing " + headerName + " header"));
                }
                auto headerValue = decode_header_value(header_pairs.get_value(headerName));
                if (!headerValue || !header_value_matches(*bodyValue.value(),
                                                         headerValue.value(),
                                                         annotation.type)) {
                    return std::unexpected(McpError::protocol_error(
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
        co_return HttpResult{400, make_unsupported_protocol_version_response(
            parsed.request.id, parsed.request.meta.protocolVersion, {MCP_VERSION})};
    }
    auto params = object_params(parsed);
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
        co_return HttpResult{200, make_result_response(id, result.encode())};
    }

    if (parsed.request.method == Methods::TOOLS_LIST ||
        parsed.request.method == Methods::RESOURCES_LIST ||
        parsed.request.method == Methods::PROMPTS_LIST) {
        ListResult result;
        result.field = parsed.request.method == Methods::TOOLS_LIST
            ? "tools" : parsed.request.method == Methods::RESOURCES_LIST ? "resources" : "prompts";
        if (result.field == "tools") {
            for (const auto& [unused, entry] : m_tools) result.items.push_back(entry.tool.encode());
        } else if (result.field == "resources") {
            for (const auto& [unused, entry] : m_resources) result.items.push_back(entry.resource.encode());
        } else {
            for (const auto& [unused, entry] : m_prompts) result.items.push_back(entry.prompt.encode());
        }
        co_return HttpResult{200, make_result_response(id, result.encode())};
    }

    if (parsed.request.method == Methods::TOOLS_CALL) {
        auto name = required(object, "name");
        if (!name) { co_return error(id, name.error()); }
        auto arguments = request_arguments(object);
        if (!arguments) { co_return error(id, arguments.error()); }
        ToolHandler handler;
        auto it = m_tools.find(name.value());
        if (it == m_tools.end()) co_return error(id, ErrorCodes::METHOD_NOT_FOUND, "Method not found", name.value(), 404);
        handler = it->second.handler;
        std::expected<std::string, McpError> value;
        co_await handler(arguments.value(), value);
        if (!value) co_return error(id, value.error());
        co_return HttpResult{200, make_result_response(id, ToolCallResult::text(value.value()).encode())};
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
        co_return HttpResult{200, make_result_response(id, ReadResourceResult::text(uri.value(), value.value(), mimeType).encode())};
    }

    if (parsed.request.method == Methods::PROMPTS_GET) {
        auto name = required(object, "name");
        if (!name) { co_return error(id, name.error()); }
        auto arguments = request_arguments(object);
        if (!arguments) { co_return error(id, arguments.error()); }
        PromptGetter getter;
        auto it = m_prompts.find(name.value());
        if (it == m_prompts.end()) co_return error(id, ErrorCodes::METHOD_NOT_FOUND, "Method not found", name.value(), 404);
        getter = it->second.getter;
        std::expected<std::string, McpError> value;
        co_await getter(name.value(), arguments.value(), value);
        if (!value) co_return error(id, value.error());
        co_return HttpResult{200, make_result_response(id, prompt_result(value.value()))};
    }

    co_return error(id, ErrorCodes::METHOD_NOT_FOUND, "Method not found", parsed.request.method, 404);
}

bool McpHttpServer::valid_origin(http::HttpRequest& request) const
{
    const std::string origin = request.header().header_pairs().get_value("Origin");
    if (origin.empty()) {
        return true;
    }
    return origin == "http://localhost" || origin == "http://127.0.0.1" ||
           origin == "https://localhost" || origin == "https://127.0.0.1";
}

SubscriptionFilter McpHttpServer::accepted_filter(
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

void McpHttpServer::reap_subscriptions()
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

void McpHttpServer::close_subscriptions()
{
    for (auto* subscription = m_subscriptions.get(); subscription; subscription = subscription->next.get()) {
        subscription->events.close();
    }
}

galay::kernel::Task<void> McpHttpServer::listen(
    http::HttpConn& conn,
    const ParsedRequest& request)
{
    auto params = object_params(request);
    if (!params) {
        co_await send_response(conn, error(request.request.id, params.error()));
        co_return;
    }
    auto requested = json::decode_member<SubscriptionFilter>(*params, "notifications")
        .transform_error([](const std::string& message) { return McpError::invalid_params(message); });
    if (!requested) {
        co_await send_response(conn, error(request.request.id, requested.error()));
        co_return;
    }

    auto operation = Operation::acquire(m_activeSubscriptions);
    if (!operation) co_return;
    // The channel requires cache-line alignment, beyond the Task frame contract.
    // Transfer aligned storage to the owner; the listener only borrows it.
    auto state = std::unique_ptr<Subscription>(new (std::nothrow) Subscription{
        request.request.id, accepted_filter(requested.value())});
    if (!state) {
        co_await send_response(conn, error(request.request.id,
            McpError::overload("subscription allocation failed")));
        co_return;
    }
    auto* subscription = state.get();
    auto submitted = submit(Command{CommandKind::Register, {}, std::move(state)});
    if (!submitted) {
        co_await send_response(conn, error(request.request.id, submitted.error()));
        co_return;
    }
    const auto registered = co_await subscription->registered.wait();
    // No timeout: the owner retains the node through registration and listening.
    if (!registered || !*registered) {
        subscription->finished.store(true, std::memory_order_release);
        wake_owner();
        co_return;
    }

    http::HttpResponseHeader header;
    header.version() = http::HttpVersion::HttpVersion_1_1;
    header.code() = http::HttpStatusCode::OK_200;
    header.header_pairs().add_header_pair("Content-Type", "text/event-stream");
    header.header_pairs().add_header_pair("Cache-Control", "no-cache");
    header.header_pairs().add_header_pair("Transfer-Encoding", "chunked");
    header.header_pairs().add_header_pair("Connection", "close");
    header.header_pairs().add_header_pair("X-Accel-Buffering", "no");
    auto writer = conn.get_writer();

    do {
        auto sendHeader = co_await writer.send_header(std::move(header));
        if (!sendHeader || !sendHeader.value()) break;
        const auto acknowledged = encode_sse_event(
            make_subscription_acknowledged_notification(subscription->id, subscription->filter));
        auto sendAcknowledged = co_await writer.send_chunk(acknowledged);
        if (!sendAcknowledged || !sendAcknowledged.value()) break;

        while (true) {
            auto event = co_await subscription->events.recv().timeout(
                std::chrono::seconds(15));
            if (!event) {
                if (galay::kernel::IOError::contains(
                        event.error().code(), galay::kernel::kTimeout)) {
                    auto keepAlive = co_await writer.send_chunk(": keep-alive\n\n");
                    if (keepAlive && keepAlive.value()) continue;
                }
                break;
            }
            auto encoded = encode_sse_event(event.value());
            auto sent = co_await writer.send_chunk(encoded);
            if (!sent || !sent.value()) {
                subscription->events.close();
                break;
            }
        }

        if (m_running.load(std::memory_order_acquire)) break;
        auto complete = encode_sse_event(make_subscription_complete_response(subscription->id));
        auto completeSent = co_await writer.send_chunk(complete);
        if (completeSent && completeSent.value()) {
            auto finalSent = co_await writer.send_chunk(std::string{}, true);
            if (!finalSent || !*finalSent) subscription->events.close();
        }
    } while (false);
    // Last node access: the owner may reclaim it as soon as finished is visible.
    // The operation lease keeps the server alive through wake_owner().
    subscription->finished.store(true, std::memory_order_release);
    wake_owner();
}

galay::kernel::Task<void> McpHttpServer::send_response(http::HttpConn& conn,
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
    auto writer = conn.get_writer();
    while (true) {
        auto sent = co_await writer.send(std::move(wire));
        if (!sent || sent.value()) break;
    }
    co_return;
}

galay::kernel::Task<void> McpHttpServer::process(http::HttpConn& conn,
                                                 http::HttpRequest& request)
{
    if (!valid_origin(request)) {
        co_await send_response(conn, error(std::nullopt, ErrorCodes::INVALID_REQUEST,
                                         "Invalid Origin", std::nullopt, 403));
        co_return;
    }
    auto parsed = parse_request(request.body_str());
    if (!parsed) {
        co_await send_response(conn, error(std::nullopt, parsed.error()));
        co_return;
    }
    auto headerValidation = validate_headers(request, parsed.value());
    if (!headerValidation) {
        co_await send_response(conn, error(parsed->request.id,
                                         ErrorCodes::HEADER_MISMATCH,
                                         "Header mismatch",
                                         headerValidation.error().details(),
                                         400));
        co_return;
    }
    if (parsed->request.meta.protocolVersion != MCP_VERSION) {
        co_await send_response(conn, HttpResult{
            400, make_unsupported_protocol_version_response(
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
    co_await send_response(conn, result);
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
    router.add_handler<http::HttpMethod::POST>("/mcp",
        [server](http::HttpConn& conn, http::HttpRequest request) -> galay::kernel::Task<void> {
            if (request.body_str().size() > server->m_policy.transport.max_http_body_bytes) {
                co_await server->send_response(conn, server->error(std::nullopt,
                    ErrorCodes::INVALID_REQUEST, "Payload too large", std::nullopt, 400));
                co_return;
            }
            co_await server->process(conn, request);
        });
    m_httpServer.start(std::move(router));
    if (!m_httpServer.is_running()) {
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
        const bool processed = process_commands();
        reap_subscriptions();
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
    while (process_commands()) { reap_subscriptions(); }
    close_subscriptions();
    while (m_subscriptions != nullptr) {
        const auto sequence = m_wakeSequence.load(std::memory_order_acquire);
        reap_subscriptions();
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
        wake_owner();
        state = LifecycleState::kStopping;
    }
    while (state == LifecycleState::kStopping) {
        m_lifecycle.wait(state, std::memory_order_acquire);
        state = m_lifecycle.load(std::memory_order_acquire);
    }
}

bool McpHttpServer::is_running() const noexcept { return m_running.load(); }

} // namespace galay::mcp::v2
