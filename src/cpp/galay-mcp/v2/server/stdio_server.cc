#include "stdio_server.h"

#include <limits>
#include <string>

namespace galay::mcp::v2 {

namespace {

std::expected<json::Json, McpError> paramsObject(const json::Json& params)
{
    if (!params.is_object()) {
        return std::unexpected(McpError::invalidParams("params must be an object"));
    }
    return params;
}

std::expected<std::string, McpError> requiredString(const json::Json& object,
                                                    const char* key)
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

std::string completeResultFromFields(std::string_view fieldsJson)
{
    auto document = JsonDocument::parse(fieldsJson);
    if (!document) {
        return R"({"resultType":"complete"})";
    }
    if (!document->root().is_object()) {
        return R"({"resultType":"complete"})";
    }
    const json::Json& fields = document->root();
    std::string out;
    auto writer = makeJsonWriter(out);
    // StreamWriter 失败粘滞：中间结果统一丢弃，由 finish() 统一检查
    std::string raw;
    (void)writer.start_object();
    (void)writer.key("resultType");
    (void)writer.string("complete");
    fields.for_each_member([&](std::string_view fieldKey, const json::Json& value) -> json::result<void> {
        if (fieldKey == "resultType") {
            return {};
        }
        raw.clear();
        auto serialized = json::stream::serialize(
            value, [&](std::string_view chunk) -> json::result<void> {
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
        return R"({"resultType":"complete"})";
    }
    return out;
}

} // namespace

McpStdioServer::McpStdioServer() = default;
McpStdioServer::~McpStdioServer() { stop(); }

void McpStdioServer::setServerInfo(std::string name, std::string version)
{
    m_serverName = std::move(name);
    m_serverVersion = std::move(version);
}

void McpStdioServer::setProductionPolicy(McpProductionPolicy policy)
{
    m_policy = std::move(policy);
}

void McpStdioServer::setStreams(std::istream& input, std::ostream& output) noexcept
{
    m_input = &input;
    m_output = &output;
}

void McpStdioServer::addTool(std::string name,
                             std::string description,
                             std::string inputSchema,
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

void McpStdioServer::addResource(std::string uri,
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

void McpStdioServer::addPrompt(std::string name,
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

std::expected<std::string, McpError> McpStdioServer::readMessage()
{
    if (m_input == nullptr) {
        return std::unexpected(McpError::invalidParams("stdio input stream is null"));
    }
    std::string line;
    if (!std::getline(*m_input, line)) {
        if (m_input->eof()) {
            return std::unexpected(McpError::connectionClosed("stdio input reached EOF"));
        }
        return std::unexpected(McpError::readError("failed to read stdio message"));
    }
    if (line.size() > m_policy.transport.max_stdio_line_bytes) {
        return std::unexpected(McpError::payloadTooLarge("stdio message exceeds configured limit"));
    }
    return line;
}

std::expected<void, McpError> McpStdioServer::writeMessage(std::string_view message)
{
    if (m_output == nullptr) {
        return std::unexpected(McpError::invalidParams("stdio output stream is null"));
    }
    if (message.size() > m_policy.transport.max_response_bytes) {
        return std::unexpected(McpError::payloadTooLarge("response exceeds configured limit"));
    }
    std::lock_guard lock(m_outputMutex);
    (*m_output) << message << '\n' << std::flush;
    if (!*m_output) {
        return std::unexpected(McpError::writeError("failed to write stdio response"));
    }
    return {};
}

std::string McpStdioServer::makeList(std::string_view field,
                                    const std::vector<std::string>& items) const
{
    ListResult result;
    result.field = std::string(field);
    result.items = items;
    result.ttlMs = 0;
    result.cacheScope = CacheScope::Private;
    return result.toJson();
}

std::string McpStdioServer::normalizePromptResult(std::string_view resultJson) const
{
    auto parsed = parseResult(resultJson);
    if (parsed) {
        return std::string(resultJson);
    }
    return completeResultFromFields(resultJson);
}

std::string McpStdioServer::error(const RequestId& id,
                                 const McpError& errorValue) const
{
    return error(id,
                 errorValue.toJsonRpcErrorCode(),
                 errorValue.message(),
                 errorValue.details().empty()
                     ? std::nullopt
                     : std::optional<std::string_view>(errorValue.details()));
}

std::string McpStdioServer::error(const RequestId& id,
                                 int code,
                                 std::string_view message,
                                 std::optional<std::string_view> data) const
{
    return makeErrorResponse(id, code, message, data);
}

std::string McpStdioServer::dispatch(const ParsedRequest& request)
{
    const RequestId& id = request.request.id;
    if (request.request.meta.protocolVersion != MCP_VERSION) {
        return makeUnsupportedProtocolVersionResponse(
            id, request.request.meta.protocolVersion, {MCP_VERSION});
    }

    auto object = paramsObject(request.request.params);
    if (!object) {
        return error(id, object.error());
    }
    const json::Json params = object.value();

    if (request.request.method == Methods::SERVER_DISCOVER) {
        DiscoverResult result;
        result.serverInfo = Implementation{.name = m_serverName, .version = m_serverVersion};
        {
            std::shared_lock lock(m_registryMutex);
            result.capabilities.tools = !m_tools.empty();
            result.capabilities.resources = !m_resources.empty();
            result.capabilities.prompts = !m_prompts.empty();
        }
        return makeResultResponse(id, result.toJson());
    }

    if (request.request.method == Methods::TOOLS_LIST ||
        request.request.method == Methods::RESOURCES_LIST ||
        request.request.method == Methods::PROMPTS_LIST) {
        std::vector<std::string> items;
        std::string field;
        {
            std::shared_lock lock(m_registryMutex);
            if (request.request.method == Methods::TOOLS_LIST) {
                field = "tools";
                items.reserve(m_tools.size());
                for (const auto& [unused, entry] : m_tools) { items.push_back(entry.tool.toJson()); }
            } else if (request.request.method == Methods::RESOURCES_LIST) {
                field = "resources";
                items.reserve(m_resources.size());
                for (const auto& [unused, entry] : m_resources) { items.push_back(entry.resource.toJson()); }
            } else {
                field = "prompts";
                items.reserve(m_prompts.size());
                for (const auto& [unused, entry] : m_prompts) { items.push_back(entry.prompt.toJson()); }
            }
        }
        return makeResultResponse(id, makeList(field, items));
    }

    if (request.request.method == Methods::TOOLS_CALL) {
        auto name = requiredString(params, "name");
        if (!name) { return error(id, name.error()); }
        json::Json arguments;
        auto argumentsResult = optionalObject(params, "arguments");
        if (!argumentsResult) { return error(id, argumentsResult.error()); }
        arguments = argumentsResult.value();
        ToolHandler handler;
        {
            std::shared_lock lock(m_registryMutex);
            const auto it = m_tools.find(name.value());
            if (it == m_tools.end()) {
                return error(id, ErrorCodes::METHOD_NOT_FOUND, "Method not found", name.value());
            }
            handler = it->second.handler;
        }
        auto value = handler(arguments);
        if (!value) { return error(id, value.error()); }
        return makeResultResponse(id, ToolCallResult::text(value.value()).toJson());
    }

    if (request.request.method == Methods::RESOURCES_READ) {
        auto uri = requiredString(params, "uri");
        if (!uri) { return error(id, uri.error()); }
        ResourceReader reader;
        std::optional<std::string> mimeType;
        {
            std::shared_lock lock(m_registryMutex);
            const auto it = m_resources.find(uri.value());
            if (it == m_resources.end()) {
                return error(id, ErrorCodes::INVALID_PARAMS, "Resource not found", uri.value());
            }
            reader = it->second.reader;
            mimeType = it->second.resource.mimeType;
        }
        auto value = reader(uri.value());
        if (!value) { return error(id, value.error()); }
        return makeResultResponse(
            id, ReadResourceResult::text(uri.value(), value.value(), mimeType).toJson());
    }

    if (request.request.method == Methods::PROMPTS_GET) {
        auto name = requiredString(params, "name");
        if (!name) { return error(id, name.error()); }
        json::Json arguments;
        auto argumentsResult = optionalObject(params, "arguments");
        if (!argumentsResult) { return error(id, argumentsResult.error()); }
        arguments = argumentsResult.value();
        PromptGetter getter;
        {
            std::shared_lock lock(m_registryMutex);
            const auto it = m_prompts.find(name.value());
            if (it == m_prompts.end()) {
                return error(id, ErrorCodes::METHOD_NOT_FOUND, "Method not found", name.value());
            }
            getter = it->second.getter;
        }
        auto value = getter(name.value(), arguments);
        if (!value) { return error(id, value.error()); }
        return makeResultResponse(id, normalizePromptResult(value.value()));
    }

    if (request.request.method == Methods::SUBSCRIPTIONS_LISTEN) {
        return error(id, ErrorCodes::METHOD_NOT_FOUND, "Method not found", request.request.method);
    }
    return error(id, ErrorCodes::METHOD_NOT_FOUND, "Method not found", request.request.method);
}

void McpStdioServer::run()
{
    m_running = true;
    while (m_running) {
        auto message = readMessage();
        if (!message) {
            if (message.error().code() == McpErrorCode::ConnectionClosed) { break; }
            (void)writeMessage(makeErrorResponse(std::nullopt,
                                                 message.error().toJsonRpcErrorCode(),
                                                 message.error().message()));
            continue;
        }
        // v2 cancellation is a stdio-only notification and has no response.
        auto notificationDocument = JsonDocument::parse(message.value());
        if (notificationDocument) {
            const json::Json& notification = notificationDocument->root();
            if (notification.is_object()) {
                const auto method = notification.at("method").as_string();
                if (method && *method == Methods::CANCELLED &&
                    !notification.contains("id")) {
                    continue;
                }
            }
        }
        auto request = parseRequest(message.value());
        if (!request) {
            (void)writeMessage(makeErrorResponse(std::nullopt,
                                                 request.error().toJsonRpcErrorCode(),
                                                 request.error().message(),
                                                 request.error().details()));
            continue;
        }
        auto response = dispatch(request.value());
        (void)writeMessage(response);
    }
    m_running = false;
}

void McpStdioServer::stop() noexcept { m_running = false; }
bool McpStdioServer::isRunning() const noexcept { return m_running.load(); }

} // namespace galay::mcp::v2
