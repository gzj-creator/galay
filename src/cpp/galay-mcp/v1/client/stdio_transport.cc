#include "stdio_transport.h"
#include "../../common/request_codec.h"
#include "../../common/mcp_log.h"

#include <iostream>

namespace galay::mcp::detail {

StdioClientTransport::StdioClientTransport(std::istream* input, std::ostream* output)
    : m_input(input)
    , m_output(output) {
}

std::expected<void, McpError> StdioClientTransport::require_streams() const {
    if (m_input == nullptr || m_output == nullptr) {
        return std::unexpected(McpError::invalid_params("stdio input/output stream is null"));
    }
    return {};
}

std::expected<void, McpError> StdioClientTransport::initialize(const std::string& clientName,
                                                               const std::string& clientVersion) {
    if (m_initialized) {
        return std::unexpected(McpError::already_initialized());
    }

    if (auto streamCheck = require_streams(); !streamCheck) {
        return std::unexpected(streamCheck.error());
    }

    m_clientName = clientName;
    m_clientVersion = clientVersion;

    InitializeParams params;
    params.protocolVersion = MCP_VERSION;
    params.clientInfo.name = clientName;
    params.clientInfo.version = clientVersion;
    params.capabilities = empty_object_string();

    auto result = send_request(Methods::INITIALIZE, params.encode());
    if (!result) {
        return std::unexpected(result.error());
    }

    auto initExp = parse_initialize_result(result.value());
    if (!initExp) {
        return std::unexpected(initExp.error());
    }

    auto initResult = std::move(initExp.value());
    m_serverInfo = std::move(initResult.serverInfo);
    m_serverCapabilities = std::move(initResult.capabilities);
    m_initialized = true;

    auto notifyResult = send_notification(Methods::INITIALIZED, empty_object_string());
    if (!notifyResult) {
        return std::unexpected(notifyResult.error());
    }

    return {};
}

std::expected<std::string, McpError> StdioClientTransport::call_tool(const std::string& toolName,
                                                                  const std::string& arguments) {
    if (!m_initialized) {
        return std::unexpected(McpError::not_initialized());
    }
    if (auto streamCheck = require_streams(); !streamCheck) {
        return std::unexpected(streamCheck.error());
    }

    ToolCallParams params;
    params.name = toolName;
    params.arguments = arguments.empty() ? empty_object_string() : arguments;

    auto result = send_request(Methods::TOOLS_CALL, params.encode());
    if (!result) {
        return std::unexpected(result.error());
    }

    return parse_tool_call_result(result.value());
}

std::expected<std::vector<Tool>, McpError> StdioClientTransport::list_tools() {
    if (!m_initialized) {
        return std::unexpected(McpError::not_initialized());
    }
    if (auto streamCheck = require_streams(); !streamCheck) {
        return std::unexpected(streamCheck.error());
    }

    auto result = send_request(Methods::TOOLS_LIST, empty_object_string());
    if (!result) {
        return std::unexpected(result.error());
    }

    return parse_list_field<Tool>(
        result.value(),
        "tools");
}

std::expected<std::vector<Resource>, McpError> StdioClientTransport::list_resources() {
    if (!m_initialized) {
        return std::unexpected(McpError::not_initialized());
    }
    if (auto streamCheck = require_streams(); !streamCheck) {
        return std::unexpected(streamCheck.error());
    }

    auto result = send_request(Methods::RESOURCES_LIST, empty_object_string());
    if (!result) {
        return std::unexpected(result.error());
    }

    return parse_list_field<Resource>(
        result.value(),
        "resources");
}

std::expected<std::string, McpError> StdioClientTransport::read_resource(const std::string& uri) {
    if (!m_initialized) {
        return std::unexpected(McpError::not_initialized());
    }
    if (auto streamCheck = require_streams(); !streamCheck) {
        return std::unexpected(streamCheck.error());
    }

    auto params = json::serialize(ResourceParams{uri});
    if (!params) {
        return std::unexpected(McpError::invalid_message(params.error()));
    }

    auto result = send_request(Methods::RESOURCES_READ, std::move(*params));
    if (!result) {
        return std::unexpected(result.error());
    }

    return parse_first_text_content(result.value(), "contents");
}

std::expected<std::vector<Prompt>, McpError> StdioClientTransport::list_prompts() {
    if (!m_initialized) {
        return std::unexpected(McpError::not_initialized());
    }
    if (auto streamCheck = require_streams(); !streamCheck) {
        return std::unexpected(streamCheck.error());
    }

    auto result = send_request(Methods::PROMPTS_LIST, empty_object_string());
    if (!result) {
        return std::unexpected(result.error());
    }

    return parse_list_field<Prompt>(
        result.value(),
        "prompts");
}

std::expected<std::string, McpError> StdioClientTransport::get_prompt(const std::string& name,
                                                                    const std::string& arguments) {
    if (!m_initialized) {
        return std::unexpected(McpError::not_initialized());
    }
    if (auto streamCheck = require_streams(); !streamCheck) {
        return std::unexpected(streamCheck.error());
    }

    auto params = json::serialize(PromptParams{name, arguments.empty() ?
        std::nullopt : std::optional<std::string>{arguments}});
    if (!params) {
        return std::unexpected(McpError::invalid_message(params.error()));
    }

    auto result = send_request(Methods::PROMPTS_GET, std::move(*params));
    if (!result) {
        return std::unexpected(result.error());
    }

    return result.value();
}

std::expected<void, McpError> StdioClientTransport::ping() {
    if (!m_initialized) {
        return std::unexpected(McpError::not_initialized());
    }
    if (auto streamCheck = require_streams(); !streamCheck) {
        return std::unexpected(streamCheck.error());
    }

    auto result = send_request(Methods::PING, empty_object_string());
    if (!result) {
        return std::unexpected(result.error());
    }

    return {};
}

std::expected<void, McpError> StdioClientTransport::disconnect() {
    m_initialized = false;
    return {};
}

bool StdioClientTransport::is_connected() const {
    return m_initialized;
}

bool StdioClientTransport::is_initialized() const {
    return m_initialized;
}

const ServerInfo& StdioClientTransport::get_server_info() const {
    return m_serverInfo;
}

const ServerCapabilities& StdioClientTransport::get_server_capabilities() const {
    return m_serverCapabilities;
}

std::expected<std::string, McpError> StdioClientTransport::send_request(std::string_view method,
                                                                      const std::optional<std::string>& params) {
    if (auto streamCheck = require_streams(); !streamCheck) {
        return std::unexpected(streamCheck.error());
    }

    const int64_t requestId = generate_request_id();
    JsonRpcRequest request;
    request.id = requestId;
    request.method = std::string(method);
    request.params = params;

    auto writeResult = write_message(request.encode());
    if (!writeResult) {
        MCP_LOG_ERROR("[stdio_client]", "write request failed method={} id={} error={}",
                      method,
                      requestId,
                      writeResult.error().message());
        return std::unexpected(writeResult.error());
    }

    while (true) {
        auto readResult = read_message();
        if (!readResult) {
            MCP_LOG_ERROR("[stdio_client]", "read response failed method={} id={} error={}",
                          method,
                          requestId,
                          readResult.error().message());
            return std::unexpected(readResult.error());
        }

        auto response_id = json::deserialize_member<std::optional<int64_t>>(readResult.value(), "id");
        if (!response_id) return std::unexpected(McpError::invalid_response(response_id.error()));
        if (!*response_id || **response_id != requestId) continue;
        auto parsed = parse_json_rpc_response(readResult.value());
        if (!parsed) return std::unexpected(parsed.error());
        const auto& response = parsed->response;
        if (response.hasError) {
            auto error = json::decode<JsonRpcError>(response.error);
            if (!error) return std::unexpected(McpError::invalid_response(error.error()));
            return std::unexpected(McpError::from_json_rpc_error(error->code, error->message,
                error->data.value_or(std::string{})));
        }
        if (response.hasResult) {
            auto result = json::serialize(response.result);
            if (!result) return std::unexpected(McpError::invalid_response(result.error()));
            return std::move(*result);
        }

        return empty_object_string();
    }
}

std::expected<void, McpError> StdioClientTransport::send_notification(std::string_view method,
                                                                    const std::optional<std::string>& params) {
    if (auto streamCheck = require_streams(); !streamCheck) {
        return std::unexpected(streamCheck.error());
    }

    JsonRpcNotification notification;
    notification.method = std::string(method);
    notification.params = params;

    return write_message(notification.encode());
}

std::expected<std::string, McpError> StdioClientTransport::read_message() {
    std::lock_guard<std::mutex> lock(m_inputMutex);

    if (m_input == nullptr) {
        return std::unexpected(McpError::invalid_params("stdio input/output stream is null"));
    }

    std::string line;
    while (std::getline(*m_input, line)) {
        if (!line.empty()) {
            return line;
        }
    }

    return std::unexpected(McpError::read_error("Failed to read from stdin"));
}

std::expected<void, McpError> StdioClientTransport::write_message(const std::string& message) {
    std::lock_guard<std::mutex> lock(m_outputMutex);

    if (m_output == nullptr) {
        return std::unexpected(McpError::invalid_params("stdio input/output stream is null"));
    }

    try {
        *m_output << message << '\n';
        m_output->flush();
        return {};
    } catch (const std::exception& e) {
        return std::unexpected(McpError::write_error(e.what()));
    }
}

int64_t StdioClientTransport::generate_request_id() {
    return m_requestIdCounter.fetch_add(1, std::memory_order_relaxed) + 1;
}

} // namespace galay::mcp::detail
