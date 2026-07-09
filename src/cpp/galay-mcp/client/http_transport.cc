#include <galay/cpp/galay-mcp/client/http_transport.h>
#include <galay/cpp/galay-mcp/common/json_parser.h>
#include <galay/cpp/galay-mcp/common/mcp_log.h>
#include <galay/cpp/galay-mcp/common/protocol_utils.h>
#include <galay/cpp/galay-kernel/common/error.h>

namespace galay::mcp::detail {

HttpClientTransport::HttpClientTransport(kernel::Runtime& runtime, McpHttpClientConfig config)
    : m_runtime(&runtime)
    , m_httpClient(std::make_unique<http::HttpClient>(
          http::HttpClientBuilder().tcpNoDelay(config.tcp_no_delay).build()))
    , m_serverUrl(std::move(config.url)) {
}

HttpClientTransport::ConnectAwaitable HttpClientTransport::connect() {
    return m_httpClient->connect(m_serverUrl);
}

HttpClientTransport::ConnectAwaitable HttpClientTransport::connect(std::string url) {
    m_serverUrl = std::move(url);
    return m_httpClient->connect(m_serverUrl);
}

HttpClientTransport::CloseAwaitable HttpClientTransport::disconnectAsync() {
    m_initialized = false;
    m_connected = false;
    return m_httpClient->close();
}

galay::kernel::Task<void> HttpClientTransport::initialize(std::string clientName,
                                          std::string clientVersion,
                                          std::expected<void, McpError>& result) {
    m_clientName = std::move(clientName);
    m_clientVersion = std::move(clientVersion);

    InitializeParams params;
    params.protocolVersion = MCP_VERSION;
    params.clientInfo.name = m_clientName;
    params.clientInfo.version = m_clientVersion;
    params.capabilities = emptyObjectString();

    std::expected<JsonString, McpError> response;
    co_await sendRequest(Methods::INITIALIZE, params.toJson(), response);
    if (!response) {
        result = std::unexpected(response.error());
        co_return;
    }

    auto initExp = parseInitializeResult(response.value());
    if (!initExp) {
        result = std::unexpected(initExp.error());
        co_return;
    }

    auto initResult = std::move(initExp.value());
    m_serverInfo = std::move(initResult.serverInfo);
    m_serverCapabilities = std::move(initResult.capabilities);
    m_initialized = true;
    result = {};
    co_return;
}

galay::kernel::Task<void> HttpClientTransport::callTool(std::string toolName,
                                        JsonString arguments,
                                        std::expected<JsonString, McpError>& result) {
    if (!m_initialized) {
        result = std::unexpected(McpError::notInitialized());
        co_return;
    }

    ToolCallParams params;
    params.name = std::move(toolName);
    params.arguments = arguments.empty() ? emptyObjectString() : std::move(arguments);

    std::expected<JsonString, McpError> response;
    co_await sendRequest(Methods::TOOLS_CALL, params.toJson(), response);
    if (!response) {
        result = std::unexpected(response.error());
        co_return;
    }

    result = parseToolCallResult(response.value());
    co_return;
}

galay::kernel::Task<void> HttpClientTransport::listTools(std::expected<std::vector<Tool>, McpError>& result) {
    if (!m_initialized) {
        result = std::unexpected(McpError::notInitialized());
        co_return;
    }

    std::expected<JsonString, McpError> response;
    co_await sendRequest(Methods::TOOLS_LIST, emptyObjectString(), response);
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

galay::kernel::Task<void> HttpClientTransport::listResources(std::expected<std::vector<Resource>, McpError>& result) {
    if (!m_initialized) {
        result = std::unexpected(McpError::notInitialized());
        co_return;
    }

    std::expected<JsonString, McpError> response;
    co_await sendRequest(Methods::RESOURCES_LIST, emptyObjectString(), response);
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

galay::kernel::Task<void> HttpClientTransport::readResource(std::string uri,
                                            std::expected<std::string, McpError>& result) {
    if (!m_initialized) {
        result = std::unexpected(McpError::notInitialized());
        co_return;
    }

    JsonWriter paramsWriter;
    paramsWriter.startObject();
    paramsWriter.key("uri");
    paramsWriter.string(std::move(uri));
    paramsWriter.endObject();

    std::expected<JsonString, McpError> response;
    co_await sendRequest(Methods::RESOURCES_READ, paramsWriter.takeString(), response);
    if (!response) {
        result = std::unexpected(response.error());
        co_return;
    }

    result = parseFirstTextContent(response.value(), "contents");
    co_return;
}

galay::kernel::Task<void> HttpClientTransport::listPrompts(std::expected<std::vector<Prompt>, McpError>& result) {
    if (!m_initialized) {
        result = std::unexpected(McpError::notInitialized());
        co_return;
    }

    std::expected<JsonString, McpError> response;
    co_await sendRequest(Methods::PROMPTS_LIST, emptyObjectString(), response);
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

galay::kernel::Task<void> HttpClientTransport::getPrompt(std::string name,
                                         JsonString arguments,
                                         std::expected<JsonString, McpError>& result) {
    if (!m_initialized) {
        result = std::unexpected(McpError::notInitialized());
        co_return;
    }

    JsonWriter paramsWriter;
    paramsWriter.startObject();
    paramsWriter.key("name");
    paramsWriter.string(std::move(name));
    if (!arguments.empty()) {
        paramsWriter.key("arguments");
        paramsWriter.raw(arguments);
    }
    paramsWriter.endObject();

    std::expected<JsonString, McpError> response;
    co_await sendRequest(Methods::PROMPTS_GET, paramsWriter.takeString(), response);
    if (!response) {
        result = std::unexpected(response.error());
        co_return;
    }

    result = response.value();
    co_return;
}

galay::kernel::Task<void> HttpClientTransport::ping(std::expected<void, McpError>& result) {
    if (!m_initialized) {
        result = std::unexpected(McpError::notInitialized());
        co_return;
    }

    std::expected<JsonString, McpError> response;
    co_await sendRequest(Methods::PING, emptyObjectString(), response);
    if (!response) {
        result = std::unexpected(response.error());
        co_return;
    }

    result = {};
    co_return;
}

bool HttpClientTransport::isConnected() const {
    return m_connected;
}

bool HttpClientTransport::isInitialized() const {
    return m_initialized;
}

const ServerInfo& HttpClientTransport::getServerInfo() const {
    return m_serverInfo;
}

const ServerCapabilities& HttpClientTransport::getServerCapabilities() const {
    return m_serverCapabilities;
}

galay::kernel::Task<void> HttpClientTransport::sendRequest(std::string_view method,
                                           std::optional<JsonString> params,
                                           std::expected<JsonString, McpError>& result) {
    const int64_t requestId = generateRequestId();
    const std::optional<std::string_view> params_view =
        params.has_value() ? std::optional<std::string_view>(*params) : std::nullopt;
    std::string requestBody = protocol::makeJsonRpcRequestBody(requestId, method, params_view);

    if (!m_connected.load()) {
        auto connectResult = co_await m_httpClient->connect(m_serverUrl);
        if (!connectResult) {
            MCP_LOG_ERROR("[http_client]", "connect failed url={} error={}",
                          m_serverUrl,
                          connectResult.error().message());
            result = std::unexpected(McpError::connectionError(
                std::string(connectResult.error().message())));
            co_return;
        }
        MCP_LOG_INFO("[http_client]", "connected url={}", m_serverUrl);
        m_connected = true;
    }

    auto sessionResult = m_httpClient->getSession();
    if (!sessionResult) {
        m_connected = false;
        MCP_LOG_ERROR("[http_client]", "session create failed method={} id={} error={}",
                      method,
                      requestId,
                      sessionResult.error().message());
        result = std::unexpected(McpError::connectionError(
            std::string(sessionResult.error().message())));
        co_return;
    }

    auto awaitable = sessionResult.value()->post(
        m_httpClient->url().path,
        requestBody,
        "application/json",
        {
            {"Host", m_httpClient->url().host + ":" + std::to_string(m_httpClient->url().port)},
            {"Content-Type", "application/json"}
        }
    );

    while (true) {
        auto httpResult = co_await awaitable;
        if (!httpResult) {
            m_connected = false;
            MCP_LOG_ERROR("[http_client]", "request failed method={} id={} error={}",
                          method,
                          requestId,
                          httpResult.error().message());
            result = std::unexpected(McpError::connectionError(
                std::string(httpResult.error().message())));
            co_return;
        }

        if (!httpResult.value()) {
            continue;
        }

        auto response = std::move(httpResult.value().value());
        if (response.header().isConnectionClose() || !response.header().isKeepAlive()) {
            m_connected = false;
        }

        if (response.header().code() != http::HttpStatusCode::OK_200) {
            MCP_LOG_WARN("[http_client]", "unexpected http status method={} id={} status={}",
                         method,
                         requestId,
                         static_cast<int>(response.header().code()));
            result = std::unexpected(McpError::connectionError(
                "HTTP error: " + std::to_string(static_cast<int>(response.header().code()))));
            co_return;
        }

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
            if (JsonHelper::getRawJson(view.result, raw)) {
                result = std::move(raw);
            } else {
                MCP_LOG_WARN("[http_client]", "result serialization failed method={} id={}", method, requestId);
                result = std::unexpected(McpError::parseError("Failed to parse result"));
            }
        } else {
            result = emptyObjectString();
        }

        co_return;
    }
}

int64_t HttpClientTransport::generateRequestId() {
    return m_requestIdCounter.fetch_add(1, std::memory_order_relaxed) + 1;
}

} // namespace galay::mcp::detail
