#include <galay/cpp/galay-mcp/v2/http_server.h>
#include <galay/cpp/galay-mcp/v2/http_headers.h>

#include <charconv>
#include <chrono>
#include <string>
#include <thread>

namespace galay::mcp::v2 {

namespace {

std::expected<JsonObject, McpError> objectParams(const ParsedRequest& request)
{
    JsonObject object;
    if (!JsonHelper::getObject(request.request.params, object)) {
        return std::unexpected(McpError::invalidParams("params must be an object"));
    }
    return object;
}

std::expected<std::string, McpError> required(const JsonObject& object, const char* key)
{
    std::string value;
    if (!JsonHelper::getString(object, key, value)) {
        return std::unexpected(McpError::invalidParams(
            std::string("missing or invalid ") + key));
    }
    return value;
}

std::expected<JsonElement, McpError> optionalObject(const JsonObject& object, const char* key)
{
    JsonElement value;
    if (!JsonHelper::getElement(object, key, value)) {
        return JsonHelper::emptyObject();
    }
    JsonObject nested;
    if (!JsonHelper::getObject(value, nested)) {
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

JsonString promptResult(std::string_view value)
{
    auto parsed = parseResult(value);
    if (parsed) {
        return std::string(value);
    }
    auto document = JsonDocument::parse(value);
    if (!document) {
        return R"({"resultType":"complete","messages":[]})";
    }
    JsonObject object;
    if (!JsonHelper::getObject(document->root(), object)) {
        return R"({"resultType":"complete","messages":[]})";
    }
    JsonWriter writer;
    writer.startObject();
    writer.key("resultType");
    writer.string("complete");
    for (auto field : object) {
        const std::string key(field.key);
        if (key == "resultType") {
            continue;
        }
        JsonString raw;
        if (!JsonHelper::getRawJson(field.value, raw)) {
            continue;
        }
        writer.key(key);
        writer.raw(raw);
    }
    writer.endObject();
    return writer.takeString();
}

} // namespace

McpHttpServer::McpHttpServer(std::string host,
                             int port,
                             std::size_t ioSchedulers,
                             std::size_t computeSchedulers,
                             bool tcpNoDelay)
    : m_host(std::move(host))
    , m_ioSchedulers(ioSchedulers)
    , m_computeSchedulers(computeSchedulers)
    , m_port(port)
    , m_tcpNoDelay(tcpNoDelay)
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
                            JsonString inputSchema,
                            ToolHandler handler)
{
    ToolEntry entry;
    entry.tool.name = std::move(name);
    entry.tool.description = std::move(description);
    entry.tool.inputSchema = std::move(inputSchema);
    entry.handler = std::move(handler);
    std::unique_lock lock(m_registryMutex);
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
    std::unique_lock lock(m_registryMutex);
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
    std::unique_lock lock(m_registryMutex);
    m_prompts.insert_or_assign(entry.prompt.name, std::move(entry));
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
            JsonElement arguments;
            if (!JsonHelper::getElement(params.value(), "arguments", arguments)) {
                arguments = JsonHelper::emptyObject();
            }
            Tool tool;
            {
                std::shared_lock lock(m_registryMutex);
                auto it = m_tools.find(name.value());
                if (it == m_tools.end()) return {};
                tool = it->second.tool;
            }
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
    http::HttpRequest& request)
{
    if (!validOrigin(request)) {
        co_return error(std::nullopt, ErrorCodes::INVALID_REQUEST,
                        "Invalid Origin", std::nullopt, 403);
    }
    const auto parsed = parseRequest(request.bodyStr());
    if (!parsed) {
        co_return error(std::nullopt, parsed.error());
    }
    auto headerValidation = validateHeaders(request, parsed.value());
    if (!headerValidation) {
        co_return error(parsed->request.id,
                        ErrorCodes::HEADER_MISMATCH,
                        "Header mismatch",
                        headerValidation.error().details(),
                        400);
    }
    if (parsed->request.meta.protocolVersion != MCP_VERSION) {
        co_return HttpResult{400, makeUnsupportedProtocolVersionResponse(
            parsed->request.id, parsed->request.meta.protocolVersion, {MCP_VERSION})};
    }
    auto params = objectParams(parsed.value());
    if (!params) {
        co_return error(parsed->request.id, params.error());
    }
    const RequestId& id = parsed->request.id;
    const JsonObject object = params.value();

    if (parsed->request.method == Methods::SERVER_DISCOVER) {
        DiscoverResult result;
        result.serverInfo = Implementation{.name = m_serverName, .version = m_serverVersion};
        {
            std::shared_lock lock(m_registryMutex);
            result.capabilities.tools = !m_tools.empty();
            result.capabilities.resources = !m_resources.empty();
            result.capabilities.prompts = !m_prompts.empty();
        }
        co_return HttpResult{200, makeResultResponse(id, result.toJson())};
    }

    if (parsed->request.method == Methods::TOOLS_LIST ||
        parsed->request.method == Methods::RESOURCES_LIST ||
        parsed->request.method == Methods::PROMPTS_LIST) {
        ListResult result;
        result.field = parsed->request.method == Methods::TOOLS_LIST
            ? "tools" : parsed->request.method == Methods::RESOURCES_LIST ? "resources" : "prompts";
        {
            std::shared_lock lock(m_registryMutex);
            if (result.field == "tools") {
                for (const auto& [unused, entry] : m_tools) result.items.push_back(entry.tool.toJson());
            } else if (result.field == "resources") {
                for (const auto& [unused, entry] : m_resources) result.items.push_back(entry.resource.toJson());
            } else {
                for (const auto& [unused, entry] : m_prompts) result.items.push_back(entry.prompt.toJson());
            }
        }
        co_return HttpResult{200, makeResultResponse(id, result.toJson())};
    }

    if (parsed->request.method == Methods::TOOLS_CALL) {
        auto name = required(object, "name");
        if (!name) { co_return error(id, name.error()); }
        auto arguments = optionalObject(object, "arguments");
        if (!arguments) { co_return error(id, arguments.error()); }
        ToolHandler handler;
        {
            std::shared_lock lock(m_registryMutex);
            auto it = m_tools.find(name.value());
            if (it == m_tools.end()) co_return error(id, ErrorCodes::METHOD_NOT_FOUND, "Method not found", name.value(), 404);
            handler = it->second.handler;
        }
        std::expected<JsonString, McpError> value;
        co_await handler(arguments.value(), value);
        if (!value) co_return error(id, value.error());
        co_return HttpResult{200, makeResultResponse(id, ToolCallResult::text(value.value()).toJson())};
    }

    if (parsed->request.method == Methods::RESOURCES_READ) {
        auto uri = required(object, "uri");
        if (!uri) { co_return error(id, uri.error()); }
        ResourceReader reader;
        std::optional<std::string> mimeType;
        {
            std::shared_lock lock(m_registryMutex);
            auto it = m_resources.find(uri.value());
            if (it == m_resources.end()) co_return error(id, ErrorCodes::INVALID_PARAMS, "Resource not found", uri.value());
            reader = it->second.reader;
            mimeType = it->second.resource.mimeType;
        }
        std::expected<std::string, McpError> value;
        co_await reader(uri.value(), value);
        if (!value) co_return error(id, value.error());
        co_return HttpResult{200, makeResultResponse(id, ReadResourceResult::text(uri.value(), value.value(), mimeType).toJson())};
    }

    if (parsed->request.method == Methods::PROMPTS_GET) {
        auto name = required(object, "name");
        if (!name) { co_return error(id, name.error()); }
        auto arguments = optionalObject(object, "arguments");
        if (!arguments) { co_return error(id, arguments.error()); }
        PromptGetter getter;
        {
            std::shared_lock lock(m_registryMutex);
            auto it = m_prompts.find(name.value());
            if (it == m_prompts.end()) co_return error(id, ErrorCodes::METHOD_NOT_FOUND, "Method not found", name.value(), 404);
            getter = it->second.getter;
        }
        std::expected<JsonString, McpError> value;
        co_await getter(name.value(), arguments.value(), value);
        if (!value) co_return error(id, value.error());
        co_return HttpResult{200, makeResultResponse(id, promptResult(value.value()))};
    }

    co_return error(id, ErrorCodes::METHOD_NOT_FOUND, "Method not found", parsed->request.method, 404);
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
    auto dispatched = co_await dispatch(request);
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
    if (m_running) return;
    m_router = std::make_unique<http::HttpRouter>();
    auto* server = this;
    m_router->addHandler<http::HttpMethod::POST>("/mcp",
        [server](http::HttpConn& conn, http::HttpRequest request) -> galay::kernel::Task<void> {
            if (request.bodyStr().size() > server->m_policy.transport.max_http_body_bytes) {
                co_await server->sendResponse(conn, server->error(std::nullopt,
                    ErrorCodes::INVALID_REQUEST, "Payload too large", std::nullopt, 400));
                co_return;
            }
            co_await server->process(conn, request);
        });
    http::HttpServerConfig config;
    config.host = m_host;
    config.port = static_cast<uint16_t>(m_port);
    config.backlog = 128;
    config.io_scheduler_count = m_ioSchedulers;
    config.compute_scheduler_count = m_computeSchedulers;
    config.tcp_no_delay = m_tcpNoDelay;
    m_httpServer = std::make_unique<http::HttpServer>(config);
    m_running = true;
    m_httpServer->start(std::move(*m_router));
    while (m_running) std::this_thread::sleep_for(std::chrono::seconds(1));
}

void McpHttpServer::stop()
{
    m_running = false;
    if (m_httpServer) {
        m_httpServer->stop();
        m_httpServer.reset();
    }
    m_router.reset();
}

bool McpHttpServer::isRunning() const noexcept { return m_running.load(); }

} // namespace galay::mcp::v2
