#include "mcp/client/http_client.h"
#include "mcp/common/json_parser.h"
#include "mcp/common/mcp_log.h"
#include "mcp/common/protocol_utils.h"

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

McpHttpClient::McpHttpClient(kernel::Runtime& runtime)
    : m_runtime(runtime) {
    m_httpClient = std::make_unique<http::HttpClient>();
}

McpHttpClient::~McpHttpClient() {
}

McpHttpClient::ConnectAwaitable McpHttpClient::connect(const std::string& url) {
    m_serverUrl = url;
    return m_httpClient->connect(url);
}

Coroutine McpHttpClient::initialize(std::string clientName,
                                    std::string clientVersion,
                                    std::expected<void, McpError>& result) {
    m_clientName = std::move(clientName);
    m_clientVersion = std::move(clientVersion);

    // 构建初始化请求
    InitializeParams params;
    params.protocolVersion = MCP_VERSION;
    params.clientInfo.name = m_clientName;
    params.clientInfo.version = m_clientVersion;
    params.capabilities = EmptyObjectString();

    std::expected<JsonString, McpError> response;
    co_await sendRequest(Methods::INITIALIZE, params.toJson(), response);

    if (!response) {
        result = std::unexpected(response.error());
        co_return;
    }

    auto docExp = JsonDocument::Parse(response.value());
    if (!docExp) {
        result = std::unexpected(McpError::initializationFailed(docExp.error().details()));
        co_return;
    }

    auto initExp = InitializeResult::fromJson(docExp.value().Root());
    if (!initExp) {
        result = std::unexpected(McpError::initializationFailed(initExp.error().message()));
        co_return;
    }

    auto initResult = std::move(initExp.value());
    m_serverInfo = std::move(initResult.serverInfo);
    m_serverCapabilities = std::move(initResult.capabilities);
    m_initialized = true;
    m_connected = true;
    result = {};

    co_return;
}

Coroutine McpHttpClient::callTool(std::string toolName,
                                  JsonString arguments,
                                  std::expected<JsonString, McpError>& result) {
    if (!m_initialized) {
        result = std::unexpected(McpError::notInitialized());
        co_return;
    }

    ToolCallParams params;
    params.name = std::move(toolName);
    params.arguments = arguments.empty() ? EmptyObjectString() : std::move(arguments);

    std::expected<JsonString, McpError> response;
    co_await sendRequest(Methods::TOOLS_CALL, params.toJson(), response);

    if (!response) {
        result = std::unexpected(response.error());
        co_return;
    }

    auto docExp = JsonDocument::Parse(response.value());
    if (!docExp) {
        result = std::unexpected(McpError::parseError(docExp.error().details()));
        co_return;
    }

    auto callExp = ToolCallResult::fromJson(docExp.value().Root());
    if (!callExp) {
        result = std::unexpected(McpError::parseError(callExp.error().message()));
        co_return;
    }

    const auto& callResult = callExp.value();
    if (callResult.isError) {
        result = std::unexpected(McpError::toolExecutionFailed("Tool returned error"));
        co_return;
    }
    if (callResult.content.empty()) {
        result = EmptyObjectString();
        co_return;
    }
    if (callResult.content[0].type == ContentType::Text) {
        result = callResult.content[0].text;
    } else {
        result = EmptyObjectString();
    }

    co_return;
}

Coroutine McpHttpClient::listTools(std::expected<std::vector<Tool>, McpError>& result) {
    if (!m_initialized) {
        result = std::unexpected(McpError::notInitialized());
        co_return;
    }

    std::expected<JsonString, McpError> response;
    co_await sendRequest(Methods::TOOLS_LIST, EmptyObjectString(), response);

    if (!response) {
        result = std::unexpected(response.error());
        co_return;
    }

    result = parseListField<Tool>(
        response.value(),
        "tools",
        [](const JsonElement& item) { return Tool::fromJson(item); });
    co_return;
}

Coroutine McpHttpClient::listResources(std::expected<std::vector<Resource>, McpError>& result) {
    if (!m_initialized) {
        result = std::unexpected(McpError::notInitialized());
        co_return;
    }

    std::expected<JsonString, McpError> response;
    co_await sendRequest(Methods::RESOURCES_LIST, EmptyObjectString(), response);

    if (!response) {
        result = std::unexpected(response.error());
        co_return;
    }

    result = parseListField<Resource>(
        response.value(),
        "resources",
        [](const JsonElement& item) { return Resource::fromJson(item); });
    co_return;
}

Coroutine McpHttpClient::readResource(std::string uri,
                                      std::expected<std::string, McpError>& result) {
    if (!m_initialized) {
        result = std::unexpected(McpError::notInitialized());
        co_return;
    }

    JsonWriter paramsWriter;
    paramsWriter.StartObject();
    paramsWriter.Key("uri");
    paramsWriter.String(std::move(uri));
    paramsWriter.EndObject();

    std::expected<JsonString, McpError> response;
    co_await sendRequest(Methods::RESOURCES_READ, paramsWriter.TakeString(), response);

    if (!response) {
        result = std::unexpected(response.error());
        co_return;
    }

    result = parseFirstTextContent(response.value(), "contents");
    co_return;
}

Coroutine McpHttpClient::listPrompts(std::expected<std::vector<Prompt>, McpError>& result) {
    if (!m_initialized) {
        result = std::unexpected(McpError::notInitialized());
        co_return;
    }

    std::expected<JsonString, McpError> response;
    co_await sendRequest(Methods::PROMPTS_LIST, EmptyObjectString(), response);

    if (!response) {
        result = std::unexpected(response.error());
        co_return;
    }

    result = parseListField<Prompt>(
        response.value(),
        "prompts",
        [](const JsonElement& item) { return Prompt::fromJson(item); });
    co_return;
}

Coroutine McpHttpClient::getPrompt(std::string name,
                                   JsonString arguments,
                                   std::expected<JsonString, McpError>& result) {
    if (!m_initialized) {
        result = std::unexpected(McpError::notInitialized());
        co_return;
    }

    JsonWriter paramsWriter;
    paramsWriter.StartObject();
    paramsWriter.Key("name");
    paramsWriter.String(std::move(name));
    if (!arguments.empty()) {
        paramsWriter.Key("arguments");
        paramsWriter.Raw(arguments);
    }
    paramsWriter.EndObject();

    std::expected<JsonString, McpError> response;
    co_await sendRequest(Methods::PROMPTS_GET, paramsWriter.TakeString(), response);

    if (!response) {
        result = std::unexpected(response.error());
        co_return;
    }

    result = response.value();
    co_return;
}

Coroutine McpHttpClient::ping(std::expected<void, McpError>& result) {
    if (!m_initialized) {
        result = std::unexpected(McpError::notInitialized());
        co_return;
    }

    std::expected<JsonString, McpError> response;
    co_await sendRequest(Methods::PING, EmptyObjectString(), response);

    if (!response) {
        result = std::unexpected(response.error());
        co_return;
    }

    result = {};
    co_return;
}

McpHttpClient::CloseAwaitable McpHttpClient::disconnect() {
    m_initialized = false;
    m_connected = false;
    return m_httpClient->close();
}

Coroutine McpHttpClient::sendRequest(std::string_view method,
                                     std::optional<JsonString> params,
                                     std::expected<JsonString, McpError>& result) {
    const int64_t requestId = generateRequestId();
    const std::optional<std::string_view> params_view =
        params.has_value() ? std::optional<std::string_view>(*params) : std::nullopt;
    std::string requestBody = protocol::makeJsonRpcRequestBody(requestId, method, params_view);

    // 如果连接断开，重新连接
    if (!m_connected.load()) {
        auto connectResult = co_await m_httpClient->connect(m_serverUrl);
        if (!connectResult) {
            MCP_LOG_ERROR("[http_client]", "connect failed url={} error={}",
                          m_serverUrl,
                          connectResult.error().message());
            result = std::unexpected(McpError::connectionError(connectResult.error().message()));
            co_return;
        }
        MCP_LOG_INFO("[http_client]", "connected url={}", m_serverUrl);
        m_connected = true;
    }

    // 发送POST请求
    auto session = m_httpClient->getSession();
    auto awaitable = session.post(
        m_httpClient->url().path,
        requestBody,
        "application/json",
        {
            {"Host", m_httpClient->url().host + ":" + std::to_string(m_httpClient->url().port)},
            {"Content-Type", "application/json"}
        }
    );

    // 循环等待直到完成
    while (true) {
        auto httpResult = co_await awaitable;

        if (!httpResult) {
            m_connected = false;
            MCP_LOG_ERROR("[http_client]", "request failed method={} id={} error={}",
                          method,
                          requestId,
                          httpResult.error().message());
            result = std::unexpected(McpError::connectionError(httpResult.error().message()));
            co_return;
        }

        if (!httpResult.value()) {
            continue;
        }

        auto response = httpResult.value().value();

        // 根据响应头判断是否需要关闭连接
        if (response.header().isConnectionClose() || !response.header().isKeepAlive()) {
            m_connected = false;
        }

        // 检查HTTP状态码
        if (response.header().code() != http::HttpStatusCode::OK_200) {
            MCP_LOG_WARN("[http_client]", "unexpected http status method={} id={} status={}",
                         method,
                         requestId,
                         static_cast<int>(response.header().code()));
            result = std::unexpected(McpError::connectionError(
                "HTTP error: " + std::to_string(static_cast<int>(response.header().code()))));
            co_return;
        }

        // 解析响应
        std::string responseBody = response.getBodyStr();
        auto parsed = parseJsonRpcResponse(responseBody);
        if (!parsed) {
            MCP_LOG_WARN("[http_client]", "json-rpc response parse failed method={} id={} error={}",
                         method,
                         requestId,
                         parsed.error().details());
            result = std::unexpected(McpError::parseError(parsed.error().details()));
            co_return;
        }

        const auto& view = parsed.value().response;
        if (view.id != requestId) {
            MCP_LOG_WARN("[http_client]", "mismatched response id expected={} actual={}", requestId, view.id);
            result = std::unexpected(McpError::invalidResponse("Mismatched response id"));
            co_return;
        }
        if (view.hasError) {
            auto errorExp = JsonRpcError::fromJson(view.error);
            if (!errorExp) {
                MCP_LOG_WARN("[http_client]", "json-rpc error parse failed method={} id={} error={}",
                             method,
                             requestId,
                             errorExp.error().message());
                result = std::unexpected(McpError::parseError(errorExp.error().message()));
                co_return;
            }
            const auto& error = errorExp.value();
            std::string details;
            if (error.data.has_value()) {
                details = error.data.value();
            }
            MCP_LOG_WARN("[http_client]", "json-rpc error method={} id={} code={} message={}",
                         method,
                         requestId,
                         error.code,
                         error.message);
            result = std::unexpected(McpError::fromJsonRpcError(
                error.code, error.message, details));
            co_return;
        }

        if (view.hasResult) {
            std::string raw;
            if (JsonHelper::GetRawJson(view.result, raw)) {
                result = std::move(raw);
            } else {
                MCP_LOG_WARN("[http_client]", "result serialization failed method={} id={}", method, requestId);
                result = std::unexpected(McpError::parseError("Failed to parse result"));
            }
        } else {
            result = EmptyObjectString();
        }

        co_return;
    }
}

int64_t McpHttpClient::generateRequestId() {
    return m_requestIdCounter.fetch_add(1, std::memory_order_relaxed) + 1;
}

} // namespace mcp
} // namespace galay
