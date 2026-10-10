#include "stdio_server.h"

#include <limits>
#include <string>

namespace galay::mcp::v2 {

namespace {

std::expected<json::Json, McpError> params_object(const json::Json& params)
{
    if (!params.is_object()) {
        return std::unexpected(McpError::invalid_params("params must be an object"));
    }
    return params;
}

std::expected<std::string, McpError> required_string(const json::Json& object,
                                                    const char* key)
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

std::string complete_result_from_fields(std::string_view fields) {
    auto value = json::merge_objects(fields, {{"resultType", {"\"complete\""}}});
    if (!value) return {};
    return std::move(*value);
}

} // namespace

McpStdioServer::McpStdioServer() = default;
McpStdioServer::~McpStdioServer() { stop(); }

void McpStdioServer::set_server_info(std::string name, std::string version)
{
    m_serverName = std::move(name);
    m_serverVersion = std::move(version);
}

void McpStdioServer::set_production_policy(McpProductionPolicy policy)
{
    m_policy = std::move(policy);
}

void McpStdioServer::set_streams(std::istream& input, std::ostream& output) noexcept
{
    m_input = &input;
    m_output = &output;
}

void McpStdioServer::add_tool(std::string name,
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

void McpStdioServer::add_resource(std::string uri,
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

void McpStdioServer::add_prompt(std::string name,
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

std::expected<std::string, McpError> McpStdioServer::read_message()
{
    if (m_input == nullptr) {
        return std::unexpected(McpError::invalid_params("stdio input stream is null"));
    }
    std::string line;
    if (!std::getline(*m_input, line)) {
        if (m_input->eof()) {
            return std::unexpected(McpError::connection_closed("stdio input reached EOF"));
        }
        return std::unexpected(McpError::read_error("failed to read stdio message"));
    }
    if (line.size() > m_policy.transport.max_stdio_line_bytes) {
        return std::unexpected(McpError::payload_too_large("stdio message exceeds configured limit"));
    }
    return line;
}

std::expected<void, McpError> McpStdioServer::write_message(std::string_view message)
{
    if (m_output == nullptr) {
        return std::unexpected(McpError::invalid_params("stdio output stream is null"));
    }
    if (message.size() > m_policy.transport.max_response_bytes) {
        return std::unexpected(McpError::payload_too_large("response exceeds configured limit"));
    }
    std::lock_guard lock(m_outputMutex);
    (*m_output) << message << '\n' << std::flush;
    if (!*m_output) {
        return std::unexpected(McpError::write_error("failed to write stdio response"));
    }
    return {};
}

std::string McpStdioServer::make_list(std::string_view field,
                                    const std::vector<std::string>& items) const
{
    ListResult result;
    result.field = std::string(field);
    result.items = items;
    result.ttlMs = 0;
    result.cacheScope = CacheScope::Private;
    return result.encode();
}

std::string McpStdioServer::normalize_prompt_result(std::string_view resultJson) const
{
    auto parsed = parse_result(resultJson);
    if (parsed) {
        return std::string(resultJson);
    }
    return complete_result_from_fields(resultJson);
}

std::string McpStdioServer::error(const RequestId& id,
                                 const McpError& errorValue) const
{
    return error(id,
                 errorValue.to_json_rpc_error_code(),
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
    return make_error_response(id, code, message, data);
}

std::string McpStdioServer::dispatch(const ParsedRequest& request)
{
    const RequestId& id = request.request.id;
    if (request.request.meta.protocolVersion != MCP_VERSION) {
        return make_unsupported_protocol_version_response(
            id, request.request.meta.protocolVersion, {MCP_VERSION});
    }

    auto object = params_object(request.request.params);
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
        return make_result_response(id, result.encode());
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
                for (const auto& [unused, entry] : m_tools) { items.push_back(entry.tool.encode()); }
            } else if (request.request.method == Methods::RESOURCES_LIST) {
                field = "resources";
                items.reserve(m_resources.size());
                for (const auto& [unused, entry] : m_resources) { items.push_back(entry.resource.encode()); }
            } else {
                field = "prompts";
                items.reserve(m_prompts.size());
                for (const auto& [unused, entry] : m_prompts) { items.push_back(entry.prompt.encode()); }
            }
        }
        return make_result_response(id, make_list(field, items));
    }

    if (request.request.method == Methods::TOOLS_CALL) {
        auto name = required_string(params, "name");
        if (!name) { return error(id, name.error()); }
        json::Json arguments;
        auto argumentsResult = request_arguments(params);
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
        return make_result_response(id, ToolCallResult::text(value.value()).encode());
    }

    if (request.request.method == Methods::RESOURCES_READ) {
        auto uri = required_string(params, "uri");
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
        return make_result_response(
            id, ReadResourceResult::text(uri.value(), value.value(), mimeType).encode());
    }

    if (request.request.method == Methods::PROMPTS_GET) {
        auto name = required_string(params, "name");
        if (!name) { return error(id, name.error()); }
        json::Json arguments;
        auto argumentsResult = request_arguments(params);
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
        return make_result_response(id, normalize_prompt_result(value.value()));
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
        auto message = read_message();
        if (!message) {
            if (message.error().code() == McpErrorCode::ConnectionClosed) { break; }
            (void)write_message(make_error_response(std::nullopt,
                                                 message.error().to_json_rpc_error_code(),
                                                 message.error().message()));
            continue;
        }
        // v2 cancellation is a stdio-only notification and has no response.
        auto notification = json::deserialize<EnvelopeFields>(message.value());
        if (notification && notification->method == Methods::CANCELLED && !notification->id) continue;
        auto request = parse_request(message.value());
        if (!request) {
            (void)write_message(make_error_response(std::nullopt,
                                                 request.error().to_json_rpc_error_code(),
                                                 request.error().message(),
                                                 request.error().details()));
            continue;
        }
        auto response = dispatch(request.value());
        (void)write_message(response);
    }
    m_running = false;
}

void McpStdioServer::stop() noexcept { m_running = false; }
bool McpStdioServer::is_running() const noexcept { return m_running.load(); }

} // namespace galay::mcp::v2
