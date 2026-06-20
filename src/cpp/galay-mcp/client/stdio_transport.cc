#include <galay/cpp/galay-mcp/client/stdio_transport.h>
#include <galay/cpp/galay-mcp/common/mcp_log.h>

#include <iostream>

namespace galay::mcp::detail {

StdioClientTransport::StdioClientTransport(std::istream* input, std::ostream* output)
    : m_input(input)
    , m_output(output) {
}

std::expected<void, McpError> StdioClientTransport::requireStreams() const {
    if (m_input == nullptr || m_output == nullptr) {
        return std::unexpected(McpError::invalidParams("stdio input/output stream is null"));
    }
    return {};
}

std::expected<void, McpError> StdioClientTransport::initialize(const std::string& clientName,
                                                               const std::string& clientVersion) {
    if (m_initialized) {
        return std::unexpected(McpError::alreadyInitialized());
    }

    if (auto streamCheck = requireStreams(); !streamCheck) {
        return std::unexpected(streamCheck.error());
    }

    m_clientName = clientName;
    m_clientVersion = clientVersion;

    InitializeParams params;
    params.protocolVersion = MCP_VERSION;
    params.clientInfo.name = clientName;
    params.clientInfo.version = clientVersion;
    params.capabilities = EmptyObjectString();

    auto result = sendRequest(Methods::INITIALIZE, params.toJson());
    if (!result) {
        return std::unexpected(result.error());
    }

    auto initExp = parseInitializeResult(result.value());
    if (!initExp) {
        return std::unexpected(initExp.error());
    }

    auto initResult = std::move(initExp.value());
    m_serverInfo = std::move(initResult.serverInfo);
    m_serverCapabilities = std::move(initResult.capabilities);
    m_initialized = true;

    auto notifyResult = sendNotification(Methods::INITIALIZED, EmptyObjectString());
    if (!notifyResult) {
        return std::unexpected(notifyResult.error());
    }

    return {};
}

std::expected<JsonString, McpError> StdioClientTransport::callTool(const std::string& toolName,
                                                                  const JsonString& arguments) {
    if (!m_initialized) {
        return std::unexpected(McpError::notInitialized());
    }
    if (auto streamCheck = requireStreams(); !streamCheck) {
        return std::unexpected(streamCheck.error());
    }

    ToolCallParams params;
    params.name = toolName;
    params.arguments = arguments.empty() ? EmptyObjectString() : arguments;

    auto result = sendRequest(Methods::TOOLS_CALL, params.toJson());
    if (!result) {
        return std::unexpected(result.error());
    }

    return parseToolCallResult(result.value());
}

std::expected<std::vector<Tool>, McpError> StdioClientTransport::listTools() {
    if (!m_initialized) {
        return std::unexpected(McpError::notInitialized());
    }
    if (auto streamCheck = requireStreams(); !streamCheck) {
        return std::unexpected(streamCheck.error());
    }

    auto result = sendRequest(Methods::TOOLS_LIST, EmptyObjectString());
    if (!result) {
        return std::unexpected(result.error());
    }

    return parseListField<Tool>(
        result.value(),
        "tools",
        [](const JsonElement& item) { return Tool::fromJson(item); });
}

std::expected<std::vector<Resource>, McpError> StdioClientTransport::listResources() {
    if (!m_initialized) {
        return std::unexpected(McpError::notInitialized());
    }
    if (auto streamCheck = requireStreams(); !streamCheck) {
        return std::unexpected(streamCheck.error());
    }

    auto result = sendRequest(Methods::RESOURCES_LIST, EmptyObjectString());
    if (!result) {
        return std::unexpected(result.error());
    }

    return parseListField<Resource>(
        result.value(),
        "resources",
        [](const JsonElement& item) { return Resource::fromJson(item); });
}

std::expected<std::string, McpError> StdioClientTransport::readResource(const std::string& uri) {
    if (!m_initialized) {
        return std::unexpected(McpError::notInitialized());
    }
    if (auto streamCheck = requireStreams(); !streamCheck) {
        return std::unexpected(streamCheck.error());
    }

    JsonWriter paramsWriter;
    paramsWriter.StartObject();
    paramsWriter.Key("uri");
    paramsWriter.String(uri);
    paramsWriter.EndObject();

    auto result = sendRequest(Methods::RESOURCES_READ, paramsWriter.TakeString());
    if (!result) {
        return std::unexpected(result.error());
    }

    return parseFirstTextContent(result.value(), "contents");
}

std::expected<std::vector<Prompt>, McpError> StdioClientTransport::listPrompts() {
    if (!m_initialized) {
        return std::unexpected(McpError::notInitialized());
    }
    if (auto streamCheck = requireStreams(); !streamCheck) {
        return std::unexpected(streamCheck.error());
    }

    auto result = sendRequest(Methods::PROMPTS_LIST, EmptyObjectString());
    if (!result) {
        return std::unexpected(result.error());
    }

    return parseListField<Prompt>(
        result.value(),
        "prompts",
        [](const JsonElement& item) { return Prompt::fromJson(item); });
}

std::expected<JsonString, McpError> StdioClientTransport::getPrompt(const std::string& name,
                                                                    const JsonString& arguments) {
    if (!m_initialized) {
        return std::unexpected(McpError::notInitialized());
    }
    if (auto streamCheck = requireStreams(); !streamCheck) {
        return std::unexpected(streamCheck.error());
    }

    JsonWriter paramsWriter;
    paramsWriter.StartObject();
    paramsWriter.Key("name");
    paramsWriter.String(name);
    if (!arguments.empty()) {
        paramsWriter.Key("arguments");
        paramsWriter.Raw(arguments);
    }
    paramsWriter.EndObject();

    auto result = sendRequest(Methods::PROMPTS_GET, paramsWriter.TakeString());
    if (!result) {
        return std::unexpected(result.error());
    }

    return result.value();
}

std::expected<void, McpError> StdioClientTransport::ping() {
    if (!m_initialized) {
        return std::unexpected(McpError::notInitialized());
    }
    if (auto streamCheck = requireStreams(); !streamCheck) {
        return std::unexpected(streamCheck.error());
    }

    auto result = sendRequest(Methods::PING, EmptyObjectString());
    if (!result) {
        return std::unexpected(result.error());
    }

    return {};
}

std::expected<void, McpError> StdioClientTransport::disconnect() {
    m_initialized = false;
    return {};
}

bool StdioClientTransport::isConnected() const {
    return m_initialized;
}

bool StdioClientTransport::isInitialized() const {
    return m_initialized;
}

const ServerInfo& StdioClientTransport::getServerInfo() const {
    return m_serverInfo;
}

const ServerCapabilities& StdioClientTransport::getServerCapabilities() const {
    return m_serverCapabilities;
}

std::expected<JsonString, McpError> StdioClientTransport::sendRequest(std::string_view method,
                                                                      const std::optional<JsonString>& params) {
    if (auto streamCheck = requireStreams(); !streamCheck) {
        return std::unexpected(streamCheck.error());
    }

    const int64_t requestId = generateRequestId();
    JsonRpcRequest request;
    request.id = requestId;
    request.method = std::string(method);
    request.params = params;

    auto writeResult = writeMessage(request.toJson());
    if (!writeResult) {
        MCP_LOG_ERROR("[stdio_client]", "write request failed method={} id={} error={}",
                      method,
                      requestId,
                      writeResult.error().message());
        return std::unexpected(writeResult.error());
    }

    while (true) {
        auto readResult = readMessage();
        if (!readResult) {
            MCP_LOG_ERROR("[stdio_client]", "read response failed method={} id={} error={}",
                          method,
                          requestId,
                          readResult.error().message());
            return std::unexpected(readResult.error());
        }

        auto docExp = JsonDocument::Parse(readResult.value());
        if (!docExp) {
            MCP_LOG_WARN("[stdio_client]", "json parse failed method={} id={} error={}",
                         method,
                         requestId,
                         docExp.error().details());
            return std::unexpected(McpError::parseError(docExp.error().details()));
        }

        JsonObject obj;
        if (!JsonHelper::GetObject(docExp.value().Root(), obj)) {
            MCP_LOG_WARN("[stdio_client]", "invalid response object method={} id={}", method, requestId);
            return std::unexpected(McpError::invalidResponse("Invalid response object"));
        }

        auto idVal = obj["id"];
        if (idVal.error() || idVal.is_null()) {
            continue;
        }
        if (!idVal.is_int64()) {
            MCP_LOG_WARN("[stdio_client]", "invalid response id method={} id={}", method, requestId);
            return std::unexpected(McpError::invalidResponse("Invalid response id"));
        }
        const int64_t responseId = idVal.get_int64().value();
        if (responseId != requestId) {
            continue;
        }

        auto errorVal = obj["error"];
        if (!errorVal.error() && !errorVal.is_null()) {
            auto errExp = JsonRpcError::fromJson(errorVal.value());
            if (!errExp) {
                MCP_LOG_WARN("[stdio_client]", "json-rpc error parse failed method={} id={} error={}",
                             method,
                             requestId,
                             errExp.error().message());
                return std::unexpected(McpError::parseError(errExp.error().message()));
            }
            std::string details;
            if (errExp.value().data.has_value()) {
                details = errExp.value().data.value();
            }
            MCP_LOG_WARN("[stdio_client]", "json-rpc error method={} id={} code={} message={}",
                         method,
                         requestId,
                         errExp.value().code,
                         errExp.value().message);
            return std::unexpected(McpError::fromJsonRpcError(
                errExp.value().code, errExp.value().message, details));
        }

        auto resultVal = obj["result"];
        if (!resultVal.error() && !resultVal.is_null()) {
            std::string raw;
            if (!JsonHelper::GetRawJson(resultVal.value(), raw)) {
                MCP_LOG_WARN("[stdio_client]", "result serialization failed method={} id={}", method, requestId);
                return std::unexpected(McpError::parseError("Failed to parse result"));
            }
            return raw;
        }

        return EmptyObjectString();
    }
}

std::expected<void, McpError> StdioClientTransport::sendNotification(std::string_view method,
                                                                    const std::optional<JsonString>& params) {
    if (auto streamCheck = requireStreams(); !streamCheck) {
        return std::unexpected(streamCheck.error());
    }

    JsonRpcNotification notification;
    notification.method = std::string(method);
    notification.params = params;

    return writeMessage(notification.toJson());
}

std::expected<std::string, McpError> StdioClientTransport::readMessage() {
    std::lock_guard<std::mutex> lock(m_inputMutex);

    if (m_input == nullptr) {
        return std::unexpected(McpError::invalidParams("stdio input/output stream is null"));
    }

    std::string line;
    while (std::getline(*m_input, line)) {
        if (!line.empty()) {
            return line;
        }
    }

    return std::unexpected(McpError::readError("Failed to read from stdin"));
}

std::expected<void, McpError> StdioClientTransport::writeMessage(const JsonString& message) {
    std::lock_guard<std::mutex> lock(m_outputMutex);

    if (m_output == nullptr) {
        return std::unexpected(McpError::invalidParams("stdio input/output stream is null"));
    }

    try {
        *m_output << message << '\n';
        m_output->flush();
        return {};
    } catch (const std::exception& e) {
        return std::unexpected(McpError::writeError(e.what()));
    }
}

int64_t StdioClientTransport::generateRequestId() {
    return m_requestIdCounter.fetch_add(1, std::memory_order_relaxed) + 1;
}

} // namespace galay::mcp::detail
