#include <galay/cpp/galay-mcp/v2/client.h>

#include <galay/cpp/galay-http/builder/http_builder.h>
#include <galay/cpp/galay-mcp/v2/http_headers.h>

#include <map>

namespace galay::mcp::v2 {

namespace {

struct StdioRequestLease {
    std::atomic_flag* flag = nullptr;

    explicit StdioRequestLease(std::atomic_flag* value) noexcept : flag(value) {}

    ~StdioRequestLease()
    {
        if (flag != nullptr) flag->clear(std::memory_order_release);
    }

    StdioRequestLease(const StdioRequestLease&) = delete;
    StdioRequestLease& operator=(const StdioRequestLease&) = delete;
};

std::expected<std::string, McpError> requiredString(const JsonObject& object, const char* key)
{
    std::string value;
    if (!JsonHelper::getString(object, key, value)) {
        return std::unexpected(McpError::invalidParams(
            std::string("missing or invalid ") + key));
    }
    return value;
}

template <typename T>
std::expected<std::vector<T>, McpError> parseItems(std::string_view result,
                                                   const char* key,
                                                   auto parser)
{
    auto document = JsonDocument::parse(result);
    if (!document) return std::unexpected(document.error());
    JsonObject object;
    if (!JsonHelper::getObject(document->root(), object)) {
        return std::unexpected(McpError::invalidResponse("result must be an object"));
    }
    JsonArray array;
    if (!JsonHelper::getArray(object, key, array)) {
        return std::unexpected(McpError::invalidResponse(std::string("missing ") + key));
    }
    std::vector<T> values;
    for (auto item : array) {
        auto value = parser(item);
        if (!value) return std::unexpected(value.error());
        values.push_back(std::move(value.value()));
    }
    return values;
}

std::expected<JsonString, McpError> firstText(std::string_view result)
{
    auto document = JsonDocument::parse(result);
    if (!document) return std::unexpected(document.error());
    JsonObject object;
    if (!JsonHelper::getObject(document->root(), object)) {
        return std::unexpected(McpError::invalidResponse("result must be an object"));
    }
    JsonArray contents;
    if (!JsonHelper::getArray(object, "contents", contents)) {
        return std::unexpected(McpError::invalidResponse("missing contents"));
    }
    for (auto item : contents) {
        JsonObject content;
        if (!JsonHelper::getObject(item, content)) continue;
        std::string text;
        if (JsonHelper::getString(content, "text", text)) return text;
    }
    return std::string{};
}

std::expected<JsonString, McpError> parseRpcResult(std::string_view body,
                                                    RequestId expectedId)
{
    auto parsed = parseResponse(body);
    if (!parsed) return std::unexpected(parsed.error());
    if (!parsed->response.hasResult) {
        JsonObject error;
        if (!JsonHelper::getObject(parsed->response.error, error)) {
            return std::unexpected(McpError::invalidResponse("invalid error response"));
        }
        int64_t code = 0;
        std::string message;
        if (!JsonHelper::getInt64(error, "code", code) ||
            !JsonHelper::getString(error, "message", message)) {
            return std::unexpected(McpError::invalidResponse("invalid error response"));
        }
        return std::unexpected(McpError::fromJsonRpcError(static_cast<int>(code), message));
    }
    if (parsed->response.id != expectedId) {
        return std::unexpected(McpError::invalidResponse("mismatched response id"));
    }
    JsonString result;
    if (!JsonHelper::getRawJson(parsed->response.result, result)) {
        return std::unexpected(McpError::invalidResponse("invalid result"));
    }
    return result;
}

std::expected<SubscriptionFilter, McpError> parseAcknowledged(
    std::string_view message, const RequestId& requestId)
{
    auto document = JsonDocument::parse(message);
    if (!document) return std::unexpected(document.error());
    JsonObject object;
    if (!JsonHelper::getObject(document->root(), object)) {
        return std::unexpected(McpError::invalidResponse("SSE message is not an object"));
    }
    std::string method;
    if (!JsonHelper::getString(object, "method", method) ||
        method != NotificationMethods::SUBSCRIPTIONS_ACKNOWLEDGED) {
        return std::unexpected(McpError::invalidResponse("subscription acknowledgement is not first"));
    }
    JsonElement paramsElement;
    JsonObject params;
    if (!JsonHelper::getElement(object, "params", paramsElement) ||
        !JsonHelper::getObject(paramsElement, params)) {
        return std::unexpected(McpError::invalidResponse("acknowledgement missing params"));
    }
    JsonElement metaElement;
    JsonObject meta;
    RequestId id;
    if (!JsonHelper::getElement(params, "_meta", metaElement) ||
        !JsonHelper::getObject(metaElement, meta) ||
        !JsonHelper::getElement(meta, "io.modelcontextprotocol/subscriptionId", metaElement)) {
        return std::unexpected(McpError::invalidResponse("acknowledgement missing subscription id"));
    }
    if (metaElement.is_int64()) id = metaElement.get_int64().value();
    else if (metaElement.is_string()) id = std::string(metaElement.get_string().value());
    else return std::unexpected(McpError::invalidResponse("invalid subscription id"));
    if (id != requestId) {
        return std::unexpected(McpError::invalidResponse("mismatched subscription id"));
    }
    JsonElement filterElement;
    if (!JsonHelper::getElement(params, "notifications", filterElement)) {
        return std::unexpected(McpError::invalidResponse("acknowledgement missing notifications"));
    }
    return SubscriptionFilter::fromJson(filterElement);
}

std::expected<bool, McpError> validateSubscriptionMessage(
    std::string_view message, const RequestId& requestId)
{
    auto document = JsonDocument::parse(message);
    if (!document) return std::unexpected(document.error());
    JsonObject object;
    if (!JsonHelper::getObject(document->root(), object)) {
        return std::unexpected(McpError::invalidResponse("SSE message is not an object"));
    }
    std::string method;
    if (JsonHelper::getString(object, "method", method)) {
        if (method == NotificationMethods::SUBSCRIPTIONS_ACKNOWLEDGED) {
            return std::unexpected(McpError::invalidResponse("duplicate subscription acknowledgement"));
        }
        if (method != NotificationMethods::TOOLS_LIST_CHANGED &&
            method != NotificationMethods::RESOURCES_LIST_CHANGED &&
            method != NotificationMethods::RESOURCES_UPDATED &&
            method != NotificationMethods::PROMPTS_LIST_CHANGED) {
            return std::unexpected(McpError::invalidResponse("unknown subscription notification"));
        }
        JsonElement paramsElement;
        JsonObject params;
        JsonElement metaElement;
        JsonObject meta;
        JsonElement subscriptionElement;
        if (!JsonHelper::getElement(object, "params", paramsElement) ||
            !JsonHelper::getObject(paramsElement, params) ||
            !JsonHelper::getElement(params, "_meta", metaElement) ||
            !JsonHelper::getObject(metaElement, meta) ||
            !JsonHelper::getElement(meta,
                                    "io.modelcontextprotocol/subscriptionId",
                                    subscriptionElement)) {
            return std::unexpected(McpError::invalidResponse(
                "subscription notification missing subscription id"));
        }
        RequestId notificationId;
        if (subscriptionElement.is_int64()) {
            notificationId = subscriptionElement.get_int64().value();
        } else if (subscriptionElement.is_string()) {
            notificationId = std::string(subscriptionElement.get_string().value());
        } else {
            return std::unexpected(McpError::invalidResponse(
                "invalid subscription notification id"));
        }
        if (notificationId != requestId) {
            return std::unexpected(McpError::invalidResponse(
                "mismatched subscription notification id"));
        }
        return true;
    }
    auto parsed = parseResponse(message);
    if (!parsed || !parsed->response.hasResult || parsed->response.id != requestId) {
        return std::unexpected(McpError::invalidResponse("invalid subscription completion"));
    }
    JsonObject result;
    if (!JsonHelper::getObject(parsed->response.result, result)) {
        return std::unexpected(McpError::invalidResponse("invalid subscription result"));
    }
    std::string resultType;
    if (!JsonHelper::getString(result, "resultType", resultType) || resultType != "complete") {
        return std::unexpected(McpError::invalidResponse("unexpected subscription result"));
    }
    return false;
}

bool hasSseContentType(http::HttpResponseHeader& header)
{
    auto value = header.headerPairs().getValue("Content-Type");
    if (value.empty()) value = header.headerPairs().getValue("content-type");
    constexpr std::string_view expected = "text/event-stream";
    if (value.size() < expected.size()) return false;
    for (std::size_t i = 0; i < expected.size(); ++i) {
        const char actual = value[i];
        if (actual != expected[i] && actual != expected[i] - ('a' - 'A')) return false;
    }
    return value.size() == expected.size() || value[expected.size()] == ';' ||
           value[expected.size()] == ' ' || value[expected.size()] == '\t';
}

} // namespace

McpStdioClient::McpStdioClient(std::istream& input,
                               std::ostream& output,
                               ClientConfig config)
    : m_input(&input), m_output(&output), m_config(std::move(config))
{
}

RequestMeta McpStdioClient::meta() const
{
    RequestMeta value;
    value.clientCapabilities = m_config.clientCapabilities;
    value.clientInfo = Implementation{.name = m_config.clientName, .version = m_config.clientVersion};
    return value;
}

std::int64_t McpStdioClient::nextId() noexcept
{
    return m_nextId.fetch_add(1, std::memory_order_relaxed) + 1;
}

std::expected<void, McpError> McpStdioClient::write(std::string_view message)
{
    if (m_input == nullptr || m_output == nullptr) {
        return std::unexpected(McpError::invalidParams("stdio stream is null"));
    }
    (*m_output) << message << '\n' << std::flush;
    if (!*m_output) return std::unexpected(McpError::writeError("stdio write failed"));
    return {};
}

std::expected<std::string, McpError> McpStdioClient::read()
{
    std::string line;
    if (m_input == nullptr || !std::getline(*m_input, line)) {
        return std::unexpected(McpError::connectionClosed("stdio input closed"));
    }
    return line;
}

std::expected<JsonString, McpError> McpStdioClient::request(std::string_view method,
                                                            std::string fields)
{
    if (m_requestActive.test_and_set(std::memory_order_acquire)) {
        return std::unexpected(McpError::overload("stdio client request already active"));
    }
    StdioRequestLease lease{&m_requestActive};
    const RequestId id = nextId();
    auto params = makeRequestParams(meta(), fields);
    if (!params) return std::unexpected(params.error());
    JsonRpcRequest request;
    request.id = id;
    request.method = std::string(method);
    request.params = std::move(params.value());
    auto writeResult = write(request.toJson());
    if (!writeResult) return std::unexpected(writeResult.error());
    while (true) {
        auto line = read();
        if (!line) return std::unexpected(line.error());
        auto response = parseResponse(line.value());
        if (!response) return std::unexpected(response.error());
        if (response->response.id != id) continue;
        return parseRpcResult(line.value(), id);
    }
}

std::expected<DiscoverResult, McpError> McpStdioClient::discover()
{
    auto value = request(Methods::SERVER_DISCOVER);
    if (!value) return std::unexpected(value.error());
    auto document = JsonDocument::parse(value.value());
    if (!document) return std::unexpected(document.error());
    return DiscoverResult::fromJson(document->root());
}

std::expected<std::vector<Tool>, McpError> McpStdioClient::listTools()
{
    auto value = request(Methods::TOOLS_LIST);
    if (!value) return std::unexpected(value.error());
    return parseItems<Tool>(value.value(), "tools", [](const JsonElement& item) { return Tool::fromJson(item); });
}

std::expected<JsonString, McpError> McpStdioClient::callTool(std::string name, JsonString arguments)
{
    JsonWriter fields;
    fields.startObject(); fields.key("name"); fields.string(name);
    fields.key("arguments"); fields.raw(arguments.empty() ? "{}" : arguments); fields.endObject();
    auto value = request(Methods::TOOLS_CALL, fields.takeString());
    if (!value) return std::unexpected(value.error());
    return value;
}

std::expected<std::vector<Resource>, McpError> McpStdioClient::listResources()
{
    auto value = request(Methods::RESOURCES_LIST);
    if (!value) return std::unexpected(value.error());
    return parseItems<Resource>(value.value(), "resources", [](const JsonElement& item) { return Resource::fromJson(item); });
}

std::expected<std::string, McpError> McpStdioClient::readResource(std::string uri)
{
    JsonWriter fields;
    fields.startObject(); fields.key("uri"); fields.string(uri); fields.endObject();
    auto value = request(Methods::RESOURCES_READ, fields.takeString());
    if (!value) return std::unexpected(value.error());
    return firstText(value.value());
}

std::expected<std::vector<Prompt>, McpError> McpStdioClient::listPrompts()
{
    auto value = request(Methods::PROMPTS_LIST);
    if (!value) return std::unexpected(value.error());
    return parseItems<Prompt>(value.value(), "prompts", [](const JsonElement& item) { return Prompt::fromJson(item); });
}

std::expected<JsonString, McpError> McpStdioClient::getPrompt(std::string name, JsonString arguments)
{
    JsonWriter fields;
    fields.startObject(); fields.key("name"); fields.string(name);
    fields.key("arguments"); fields.raw(arguments.empty() ? "{}" : arguments); fields.endObject();
    return request(Methods::PROMPTS_GET, fields.takeString());
}

McpHttpClient::McpHttpClient(kernel::Runtime& runtime,
                             std::string url,
                             ClientConfig config)
    : m_runtime(&runtime)
    , m_client(std::make_unique<http::HttpClient>(http::HttpClientBuilder().build()))
    , m_url(std::move(url))
    , m_config(std::move(config))
{
}

McpHttpClient::ConnectAwaitable McpHttpClient::connect()
{
    return m_client->connect(m_url);
}

McpHttpClient::CloseAwaitable McpHttpClient::close()
{
    m_connected = false;
    return m_client->close();
}

RequestMeta McpHttpClient::meta() const
{
    RequestMeta value;
    value.clientCapabilities = m_config.clientCapabilities;
    value.clientInfo = Implementation{.name = m_config.clientName, .version = m_config.clientVersion};
    return value;
}

std::int64_t McpHttpClient::nextId() noexcept
{
    return m_nextId.fetch_add(1, std::memory_order_relaxed) + 1;
}

kernel::Task<void> McpHttpClient::request(std::string method,
                                          std::string fields,
                                          std::expected<JsonString, McpError>& result)
{
    if (!m_connected.load()) {
        auto connected = co_await m_client->connect(m_url);
        if (!connected) {
            result = std::unexpected(McpError::connectionError(std::string(connected.error().message())));
            co_return;
        }
        m_connected = true;
    }
    const RequestId id = nextId();
    auto params = makeRequestParams(meta(), fields);
    if (!params) { result = std::unexpected(params.error()); co_return; }
    JsonRpcRequest message;
    message.id = id;
    message.method = method;
    message.params = std::move(params.value());
    const std::string body = message.toJson();
    auto session = m_client->getSession();
    if (!session) { result = std::unexpected(McpError::connectionError(session.error().message())); co_return; }
    std::string nameHeader;
    if (method == Methods::TOOLS_CALL || method == Methods::PROMPTS_GET) {
        auto doc = JsonDocument::parse(fields);
        JsonObject object;
        if (doc && JsonHelper::getObject(doc->root(), object)) {
            (void)JsonHelper::getString(object, "name", nameHeader);
        }
    } else if (method == Methods::RESOURCES_READ) {
        auto doc = JsonDocument::parse(fields);
        JsonObject object;
        if (doc && JsonHelper::getObject(doc->root(), object)) {
            (void)JsonHelper::getString(object, "uri", nameHeader);
        }
    }
    std::map<std::string, std::string> headers{
        {"Host", m_client->url().host + ":" + std::to_string(m_client->url().port)},
        {"Accept", "application/json, text/event-stream"},
        {"MCP-Protocol-Version", MCP_VERSION},
        {"Mcp-Method", method}};
    if (!nameHeader.empty()) headers.emplace("Mcp-Name", encodeHeaderValue(nameHeader));
    if (method == Methods::TOOLS_CALL) {
        auto fieldsDocument = JsonDocument::parse(fields);
        JsonObject fieldsObject;
        if (fieldsDocument && JsonHelper::getObject(fieldsDocument->root(), fieldsObject)) {
            std::string toolName;
            JsonElement arguments;
            if (JsonHelper::getString(fieldsObject, "name", toolName) &&
                JsonHelper::getElement(fieldsObject, "arguments", arguments)) {
                std::optional<Tool> tool;
                const auto definitions = m_toolDefinitions.load(std::memory_order_acquire);
                auto it = definitions->find(toolName);
                if (it != definitions->end()) tool = it->second;
                if (tool) {
                    auto annotations = toolHeaderAnnotations(*tool);
                    if (!annotations) {
                        result = std::unexpected(annotations.error());
                        co_return;
                    }
                    for (const auto& annotation : annotations.value()) {
                        auto value = argumentHeaderValue(arguments, annotation);
                        if (!value) {
                            result = std::unexpected(value.error());
                            co_return;
                        }
                        if (value.value()) {
                            headers.emplace("Mcp-Param-" + annotation.name,
                                            encodeHeaderValue(*value.value()));
                        }
                    }
                }
            }
        }
    }
    auto response = session.value()->post(m_client->url().path, body, "application/json", headers);
    while (true) {
        auto received = co_await response;
        if (!received) { result = std::unexpected(McpError::connectionError(std::string(received.error().message()))); co_return; }
        if (!received.value()) continue;
        auto value = std::move(received.value().value());
        if (value.header().code() != http::HttpStatusCode::OK_200) {
            auto rpcError = parseRpcResult(value.getBodyStr(), id);
            if (!rpcError) {
                result = std::unexpected(rpcError.error());
            } else {
                result = std::unexpected(McpError::connectionError(
                    "HTTP error: " + std::to_string(static_cast<int>(value.header().code()))));
            }
            co_return;
        }
        result = parseRpcResult(value.getBodyStr(), id);
        co_return;
    }
}

kernel::Task<void> McpHttpClient::discover(std::expected<DiscoverResult, McpError>& result)
{
    std::expected<JsonString, McpError> value;
    co_await request(Methods::SERVER_DISCOVER, "{}", value);
    if (!value) { result = std::unexpected(value.error()); co_return; }
    auto doc = JsonDocument::parse(value.value());
    if (!doc) { result = std::unexpected(doc.error()); co_return; }
    result = DiscoverResult::fromJson(doc->root());
}

kernel::Task<void> McpHttpClient::listTools(std::expected<std::vector<Tool>, McpError>& result)
{
    std::expected<JsonString, McpError> value;
    co_await request(Methods::TOOLS_LIST, "{}", value);
    if (!value) { result = std::unexpected(value.error()); co_return; }
    auto parsed = parseItems<Tool>(value.value(), "tools", [](const JsonElement& item) { return Tool::fromJson(item); });
    if (!parsed) { result = std::unexpected(parsed.error()); co_return; }
    std::vector<Tool> valid;
    for (auto& tool : parsed.value()) {
        auto annotations = toolHeaderAnnotations(tool);
        if (!annotations) continue;
        valid.push_back(std::move(tool));
    }
    auto definitions = std::make_shared<McpHttpClient::ToolDefinitions>();
    definitions->reserve(valid.size());
    for (const auto& tool : valid) definitions->insert_or_assign(tool.name, tool);
    std::shared_ptr<const McpHttpClient::ToolDefinitions> published = std::move(definitions);
    m_toolDefinitions.store(std::move(published), std::memory_order_release);
    result = std::move(valid);
}

kernel::Task<void> McpHttpClient::callTool(std::string name, JsonString arguments,
                                           std::expected<JsonString, McpError>& result)
{
    JsonWriter fields;
    fields.startObject(); fields.key("name"); fields.string(name); fields.key("arguments"); fields.raw(arguments.empty() ? "{}" : arguments); fields.endObject();
    co_await request(Methods::TOOLS_CALL, fields.takeString(), result);
}

kernel::Task<void> McpHttpClient::listResources(std::expected<std::vector<Resource>, McpError>& result)
{
    std::expected<JsonString, McpError> value;
    co_await request(Methods::RESOURCES_LIST, "{}", value);
    if (!value) { result = std::unexpected(value.error()); co_return; }
    result = parseItems<Resource>(value.value(), "resources", [](const JsonElement& item) { return Resource::fromJson(item); });
}

kernel::Task<void> McpHttpClient::readResource(std::string uri,
                                               std::expected<std::string, McpError>& result)
{
    JsonWriter fields;
    fields.startObject(); fields.key("uri"); fields.string(uri); fields.endObject();
    std::expected<JsonString, McpError> value;
    co_await request(Methods::RESOURCES_READ, fields.takeString(), value);
    if (!value) { result = std::unexpected(value.error()); co_return; }
    result = firstText(value.value());
}

kernel::Task<void> McpHttpClient::listPrompts(std::expected<std::vector<Prompt>, McpError>& result)
{
    std::expected<JsonString, McpError> value;
    co_await request(Methods::PROMPTS_LIST, "{}", value);
    if (!value) { result = std::unexpected(value.error()); co_return; }
    result = parseItems<Prompt>(value.value(), "prompts", [](const JsonElement& item) { return Prompt::fromJson(item); });
}

kernel::Task<void> McpHttpClient::getPrompt(std::string name, JsonString arguments,
                                            std::expected<JsonString, McpError>& result)
{
    JsonWriter fields;
    fields.startObject(); fields.key("name"); fields.string(name); fields.key("arguments"); fields.raw(arguments.empty() ? "{}" : arguments); fields.endObject();
    co_await request(Methods::PROMPTS_GET, fields.takeString(), result);
}

kernel::Task<void> McpHttpClient::listen(
    SubscriptionFilter filter,
    SubscriptionCallback callback,
    std::expected<SubscriptionFilter, McpError>& result)
{
    if (!callback) {
        result = std::unexpected(McpError::invalidParams("subscription callback is empty"));
        co_return;
    }

    http::HttpClient listenClient(http::HttpClientBuilder().build());
    auto connected = co_await listenClient.connect(m_url);
    if (!connected) {
        result = std::unexpected(McpError::connectionError(
            std::string(connected.error().message())));
        co_return;
    }
    auto session = listenClient.getSession(64 * 1024);
    if (!session) {
        result = std::unexpected(McpError::connectionError(session.error().message()));
        co_return;
    }

    const RequestId requestId = nextId();
    auto params = makeRequestParams(meta(), [&filter] {
        JsonWriter fields;
        fields.startObject();
        fields.key("notifications");
        fields.raw(filter.toJson());
        fields.endObject();
        return fields.takeString();
    }());
    if (!params) {
        result = std::unexpected(params.error());
        co_return;
    }
    JsonRpcRequest request;
    request.id = requestId;
    request.method = Methods::SUBSCRIPTIONS_LISTEN;
    request.params = std::move(params.value());

    std::string requestBody = request.toJson();
    http::HttpRequest httpRequest;
    http::HttpRequestHeader httpHeader;
    httpHeader.method() = http::HttpMethod::POST;
    httpHeader.uri() = listenClient.url().path;
    httpHeader.version() = http::HttpVersion::HttpVersion_1_1;
    httpHeader.headerPairs().addHeaderPair("Host", listenClient.url().host + ":" +
                                           std::to_string(listenClient.url().port));
    httpHeader.headerPairs().addHeaderPair("Accept", "application/json, text/event-stream");
    httpHeader.headerPairs().addHeaderPair("Content-Type", "application/json");
    httpHeader.headerPairs().addHeaderPair("Connection", "close");
    httpHeader.headerPairs().addHeaderPair("MCP-Protocol-Version", MCP_VERSION);
    httpHeader.headerPairs().addHeaderPair("Mcp-Method", Methods::SUBSCRIPTIONS_LISTEN);
    httpHeader.headerPairs().addHeaderPair("Content-Length", std::to_string(requestBody.size()));
    httpRequest.setHeader(std::move(httpHeader));
    httpRequest.setBodyStr(std::move(requestBody));

    auto writer = session.value()->getWriter();
    auto sent = co_await writer.sendRequest(httpRequest);
    if (!sent || !sent.value()) {
        result = std::unexpected(McpError::connectionError(
            sent ? "failed to send subscription request" : sent.error().message()));
        co_return;
    }
    http::HttpResponseHeader responseHeader;
    auto headerResult = co_await session.value()->getResponseHeader(responseHeader);
    if (!headerResult || !headerResult.value() ||
        responseHeader.code() != http::HttpStatusCode::OK_200 ||
        !hasSseContentType(responseHeader)) {
        result = std::unexpected(McpError::invalidResponse("invalid subscription SSE response"));
        co_return;
    }

    http::ChunkParser chunkParser;
    std::string chunk;
    std::string sseBuffer;
    bool acknowledged = false;
    bool completed = false;
    while (!completed) {
        chunk.clear();
        auto chunkResult = co_await session.value()->getNextChunk(chunk, chunkParser);
        if (!chunkResult) {
            result = std::unexpected(McpError::connectionClosed(chunkResult.error().message()));
            co_return;
        }
        sseBuffer += chunk;
        for (;;) {
            std::size_t delimiter = sseBuffer.find("\n\n");
            std::size_t delimiterSize = 2;
            const std::size_t crlf = sseBuffer.find("\r\n\r\n");
            if (crlf != std::string::npos && (delimiter == std::string::npos || crlf < delimiter)) {
                delimiter = crlf;
                delimiterSize = 4;
            }
            if (delimiter == std::string::npos) break;
            const std::string event = sseBuffer.substr(0, delimiter + delimiterSize);
            sseBuffer.erase(0, delimiter + delimiterSize);
            auto parsedEvent = parseSseEvent(event);
            if (!parsedEvent) {
                result = std::unexpected(parsedEvent.error());
                co_return;
            }
            if (!parsedEvent->has_value()) continue;
            const auto message = std::move(parsedEvent->value());
            if (!acknowledged) {
                auto accepted = parseAcknowledged(message, requestId);
                if (!accepted) {
                    result = std::unexpected(accepted.error());
                    co_return;
                }
                result = std::move(accepted.value());
                acknowledged = true;
                continue;
            }
            auto messageState = validateSubscriptionMessage(message, requestId);
            if (!messageState) {
                result = std::unexpected(messageState.error());
                co_return;
            }
            completed = !messageState.value();
            if (!completed && !callback(std::string(message))) {
                auto closed = co_await listenClient.close();
                if (!closed) {
                    result = std::unexpected(McpError::connectionError(
                        std::string(closed.error().message())));
                }
                co_return;
            }
            if (completed) break;
        }
    }
    co_return;
}

} // namespace galay::mcp::v2
