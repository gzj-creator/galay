#include "stdio_transport.h"
#include "../../common/mcp_log.h"

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
    params.capabilities = emptyObjectString();

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

    auto notifyResult = sendNotification(Methods::INITIALIZED, emptyObjectString());
    if (!notifyResult) {
        return std::unexpected(notifyResult.error());
    }

    return {};
}

std::expected<std::string, McpError> StdioClientTransport::callTool(const std::string& toolName,
                                                                  const std::string& arguments) {
    if (!m_initialized) {
        return std::unexpected(McpError::notInitialized());
    }
    if (auto streamCheck = requireStreams(); !streamCheck) {
        return std::unexpected(streamCheck.error());
    }

    ToolCallParams params;
    params.name = toolName;
    params.arguments = arguments.empty() ? emptyObjectString() : arguments;

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

    auto result = sendRequest(Methods::TOOLS_LIST, emptyObjectString());
    if (!result) {
        return std::unexpected(result.error());
    }

    return parseListField<Tool>(
        result.value(),
        "tools",
        [](const json::Json& item) { return Tool::fromJson(item); });
}

std::expected<std::vector<Resource>, McpError> StdioClientTransport::listResources() {
    if (!m_initialized) {
        return std::unexpected(McpError::notInitialized());
    }
    if (auto streamCheck = requireStreams(); !streamCheck) {
        return std::unexpected(streamCheck.error());
    }

    auto result = sendRequest(Methods::RESOURCES_LIST, emptyObjectString());
    if (!result) {
        return std::unexpected(result.error());
    }

    return parseListField<Resource>(
        result.value(),
        "resources",
        [](const json::Json& item) { return Resource::fromJson(item); });
}

std::expected<std::string, McpError> StdioClientTransport::readResource(const std::string& uri) {
    if (!m_initialized) {
        return std::unexpected(McpError::notInitialized());
    }
    if (auto streamCheck = requireStreams(); !streamCheck) {
        return std::unexpected(streamCheck.error());
    }

    std::string params;
    auto paramsWriter = makeJsonWriter(params);
    // StreamWriter 失败粘滞：中间结果统一丢弃，由 finish() 统一检查
    (void)paramsWriter.start_object();
    (void)paramsWriter.key("uri");
    (void)paramsWriter.string(uri);
    (void)paramsWriter.end_object();
    if (!paramsWriter.finish()) {
        return std::unexpected(McpError::invalidMessage("failed to encode JSON: " + paramsWriter.finish().error()));
    }

    auto result = sendRequest(Methods::RESOURCES_READ, std::move(params));
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

    auto result = sendRequest(Methods::PROMPTS_LIST, emptyObjectString());
    if (!result) {
        return std::unexpected(result.error());
    }

    return parseListField<Prompt>(
        result.value(),
        "prompts",
        [](const json::Json& item) { return Prompt::fromJson(item); });
}

std::expected<std::string, McpError> StdioClientTransport::getPrompt(const std::string& name,
                                                                    const std::string& arguments) {
    if (!m_initialized) {
        return std::unexpected(McpError::notInitialized());
    }
    if (auto streamCheck = requireStreams(); !streamCheck) {
        return std::unexpected(streamCheck.error());
    }

    std::string params;
    auto paramsWriter = makeJsonWriter(params);
    // StreamWriter 失败粘滞：中间结果统一丢弃，由 finish() 统一检查
    (void)paramsWriter.start_object();
    (void)paramsWriter.key("name");
    (void)paramsWriter.string(name);
    if (!arguments.empty()) {
        (void)paramsWriter.key("arguments");
        (void)paramsWriter.raw(arguments);
    }
    (void)paramsWriter.end_object();
    if (!paramsWriter.finish()) {
        return std::unexpected(McpError::invalidMessage("failed to encode JSON: " + paramsWriter.finish().error()));
    }

    auto result = sendRequest(Methods::PROMPTS_GET, std::move(params));
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

    auto result = sendRequest(Methods::PING, emptyObjectString());
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

std::expected<std::string, McpError> StdioClientTransport::sendRequest(std::string_view method,
                                                                      const std::optional<std::string>& params) {
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

        auto docExp = JsonDocument::parse(readResult.value());
        if (!docExp) {
            MCP_LOG_WARN("[stdio_client]", "json parse failed method={} id={} error={}",
                         method,
                         requestId,
                         docExp.error().details());
            return std::unexpected(McpError::parseError(docExp.error().details()));
        }

        json::Json obj = docExp.value().root();
        if (!obj.is_object()) {
            MCP_LOG_WARN("[stdio_client]", "invalid response object method={} id={}", method, requestId);
            return std::unexpected(McpError::invalidResponse("Invalid response object"));
        }

        auto idVal = obj["id"];
        if (!idVal.valid() || idVal.is_null()) {
            continue;
        }
        auto responseIdVal = idVal.as_int64();
        if (!responseIdVal) {
            MCP_LOG_WARN("[stdio_client]", "invalid response id method={} id={}", method, requestId);
            return std::unexpected(McpError::invalidResponse("Invalid response id"));
        }
        const int64_t responseId = responseIdVal.value();
        if (responseId != requestId) {
            continue;
        }

        auto errorVal = obj["error"];
        if (errorVal.valid() && !errorVal.is_null()) {
            auto errExp = JsonRpcError::fromJson(errorVal);
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
        if (resultVal.valid() && !resultVal.is_null()) {
            std::string raw;
            auto serialized = json::stream::serialize(resultVal, [&](std::string_view chunk) -> json::result<void> {
                raw.append(chunk);
                return {};
            });
            if (!serialized) {
                MCP_LOG_WARN("[stdio_client]", "result serialization failed method={} id={}", method, requestId);
                return std::unexpected(McpError::parseError("Failed to parse result"));
            }
            return raw;
        }

        return emptyObjectString();
    }
}

std::expected<void, McpError> StdioClientTransport::sendNotification(std::string_view method,
                                                                    const std::optional<std::string>& params) {
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

std::expected<void, McpError> StdioClientTransport::writeMessage(const std::string& message) {
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
