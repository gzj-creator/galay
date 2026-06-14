#include "mcp/client/stdio_client.h"
#include "mcp/common/mcp_log.h"

namespace galay {
namespace mcp {

namespace {

const JsonString& EmptyObjectString() {
    static const JsonString kEmptyObject = "{}";
    return kEmptyObject;
}

template <typename T, typename ParseFn>
std::expected<std::vector<T>, McpError> parseListField(std::string_view body,
                                                       const char* fieldName,
                                                       ParseFn&& parseFn) {
    auto docExp = JsonDocument::Parse(body);
    if (!docExp) {
        return std::unexpected(McpError::parseError(docExp.error().details()));
    }

    JsonObject obj;
    if (!JsonHelper::GetObject(docExp.value().Root(), obj)) {
        return std::unexpected(McpError::parseError("Expected JSON object"));
    }

    std::vector<T> values;
    JsonArray arr;
    if (!JsonHelper::GetArray(obj, fieldName, arr)) {
        return values;
    }

    for (auto item : arr) {
        auto parsed = parseFn(item);
        if (!parsed) {
            return std::unexpected(McpError::parseError(parsed.error().message()));
        }
        values.emplace_back(std::move(parsed.value()));
    }

    return values;
}

std::expected<std::string, McpError> parseFirstTextContent(std::string_view body,
                                                           const char* fieldName) {
    auto docExp = JsonDocument::Parse(body);
    if (!docExp) {
        return std::unexpected(McpError::parseError(docExp.error().details()));
    }

    JsonObject obj;
    if (!JsonHelper::GetObject(docExp.value().Root(), obj)) {
        return std::unexpected(McpError::parseError("Expected JSON object"));
    }

    JsonArray arr;
    if (!JsonHelper::GetArray(obj, fieldName, arr)) {
        return std::string();
    }

    for (auto item : arr) {
        auto contentExp = Content::fromJson(item);
        if (!contentExp) {
            return std::unexpected(McpError::parseError(contentExp.error().message()));
        }
        if (contentExp.value().type == ContentType::Text) {
            return contentExp.value().text;
        }
    }

    return std::string();
}

} // namespace

McpStdioClient::McpStdioClient()
    : m_initialized(false)
    , m_requestIdCounter(0)
    , m_input(&std::cin)
    , m_output(&std::cout) {
}

McpStdioClient::~McpStdioClient() {
    disconnect();
}

std::expected<void, McpError> McpStdioClient::initialize(const std::string& clientName,
                                                         const std::string& clientVersion) {
    if (m_initialized) {
        return std::unexpected(McpError::alreadyInitialized());
    }

    m_clientName = clientName;
    m_clientVersion = clientVersion;

    // 构建初始化请求
    InitializeParams params;
    params.protocolVersion = MCP_VERSION;
    params.clientInfo.name = clientName;
    params.clientInfo.version = clientVersion;
    params.capabilities = EmptyObjectString();

    auto result = sendRequest(Methods::INITIALIZE, params.toJson());
    if (!result) {
        return std::unexpected(result.error());
    }

    auto docExp = JsonDocument::Parse(result.value());
    if (!docExp) {
        return std::unexpected(McpError::initializationFailed(docExp.error().details()));
    }

    auto initExp = InitializeResult::fromJson(docExp.value().Root());
    if (!initExp) {
        return std::unexpected(McpError::initializationFailed(initExp.error().message()));
    }

    auto initResult = std::move(initExp.value());
    m_serverInfo = std::move(initResult.serverInfo);
    m_serverCapabilities = std::move(initResult.capabilities);
    m_initialized = true;

    // 发送initialized通知
    auto notifyResult = sendNotification(Methods::INITIALIZED, EmptyObjectString());
    if (!notifyResult) {
        return std::unexpected(notifyResult.error());
    }

    return {};
}

std::expected<JsonString, McpError> McpStdioClient::callTool(const std::string& toolName,
                                                             const JsonString& arguments) {
    if (!m_initialized) {
        return std::unexpected(McpError::notInitialized());
    }

    ToolCallParams params;
    params.name = toolName;
    params.arguments = arguments.empty() ? EmptyObjectString() : arguments;

    auto result = sendRequest(Methods::TOOLS_CALL, params.toJson());
    if (!result) {
        return std::unexpected(result.error());
    }

    auto docExp = JsonDocument::Parse(result.value());
    if (!docExp) {
        return std::unexpected(McpError::parseError(docExp.error().details()));
    }

    auto callExp = ToolCallResult::fromJson(docExp.value().Root());
    if (!callExp) {
        return std::unexpected(McpError::parseError(callExp.error().message()));
    }

    const auto& callResult = callExp.value();
    if (callResult.isError) {
        return std::unexpected(McpError::toolExecutionFailed("Tool returned error"));
    }

    if (callResult.content.empty()) {
        return EmptyObjectString();
    }

    if (callResult.content[0].type == ContentType::Text) {
        return callResult.content[0].text;
    }

    return EmptyObjectString();
}

std::expected<std::vector<Tool>, McpError> McpStdioClient::listTools() {
    if (!m_initialized) {
        return std::unexpected(McpError::notInitialized());
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

std::expected<std::vector<Resource>, McpError> McpStdioClient::listResources() {
    if (!m_initialized) {
        return std::unexpected(McpError::notInitialized());
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

std::expected<std::string, McpError> McpStdioClient::readResource(const std::string& uri) {
    if (!m_initialized) {
        return std::unexpected(McpError::notInitialized());
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

std::expected<std::vector<Prompt>, McpError> McpStdioClient::listPrompts() {
    if (!m_initialized) {
        return std::unexpected(McpError::notInitialized());
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

std::expected<JsonString, McpError> McpStdioClient::getPrompt(const std::string& name,
                                                              const JsonString& arguments) {
    if (!m_initialized) {
        return std::unexpected(McpError::notInitialized());
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

std::expected<void, McpError> McpStdioClient::ping() {
    if (!m_initialized) {
        return std::unexpected(McpError::notInitialized());
    }

    auto result = sendRequest(Methods::PING, EmptyObjectString());
    if (!result) {
        return std::unexpected(result.error());
    }

    return {};
}

void McpStdioClient::disconnect() {
    m_initialized = false;
}

bool McpStdioClient::isInitialized() const {
    return m_initialized;
}

const ServerInfo& McpStdioClient::getServerInfo() const {
    return m_serverInfo;
}

const ServerCapabilities& McpStdioClient::getServerCapabilities() const {
    return m_serverCapabilities;
}

std::expected<JsonString, McpError> McpStdioClient::sendRequest(std::string_view method,
                                                                const std::optional<JsonString>& params) {
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

    // 读取响应（跳过通知）
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
            // 通知消息，忽略
            continue;
        }
        if (!idVal.is_int64()) {
            MCP_LOG_WARN("[stdio_client]", "invalid response id method={} id={}", method, requestId);
            return std::unexpected(McpError::invalidResponse("Invalid response id"));
        }
        const int64_t responseId = idVal.get_int64().value();
        if (responseId != requestId) {
            // 忽略其他请求的响应，继续等待当前 request id。
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

std::expected<void, McpError> McpStdioClient::sendNotification(std::string_view method,
                                                               const std::optional<JsonString>& params) {
    JsonRpcNotification notification;
    notification.method = std::string(method);
    notification.params = params;

    return writeMessage(notification.toJson());
}

std::expected<std::string, McpError> McpStdioClient::readMessage() {
    std::lock_guard<std::mutex> lock(m_inputMutex);

    std::string line;
    while (std::getline(*m_input, line)) {
        if (!line.empty()) {
            return line;
        }
    }

    return std::unexpected(McpError::readError("Failed to read from stdin"));
}

std::expected<void, McpError> McpStdioClient::writeMessage(const JsonString& message) {
    std::lock_guard<std::mutex> lock(m_outputMutex);

    try {
        *m_output << message << '\n';
        m_output->flush();
        return {};
    } catch (const std::exception& e) {
        return std::unexpected(McpError::writeError(e.what()));
    }
}

int64_t McpStdioClient::generateRequestId() {
    return m_requestIdCounter.fetch_add(1, std::memory_order_relaxed) + 1;
}

} // namespace mcp
} // namespace galay
