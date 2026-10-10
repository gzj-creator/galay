#include "http_transport.h"
#include "../../common/request_codec.h"
#include "../../common/mcp_log.h"
#include "../../common/protocol_utils.h"
#include "../../../galay-kernel/common/error.h"

namespace galay::mcp::detail {

HttpClientTransport::HttpClientTransport(kernel::Runtime& runtime, McpHttpClientConfig config)
    : m_runtime(&runtime)
    , m_httpClient(std::make_unique<http::HttpClient>(
          http::HttpClientBuilder().tcp_no_delay(config.tcp_no_delay).build()))
    , m_serverUrl(std::move(config.url)) {
}

HttpClientTransport::ConnectAwaitable HttpClientTransport::connect() {
    return m_httpClient->connect(m_serverUrl);
}

HttpClientTransport::ConnectAwaitable HttpClientTransport::connect(std::string url) {
    m_serverUrl = std::move(url);
    return m_httpClient->connect(m_serverUrl);
}

HttpClientTransport::CloseAwaitable HttpClientTransport::disconnect_async() {
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
    params.capabilities = empty_object_string();

    std::expected<std::string, McpError> response;
    co_await send_request(Methods::INITIALIZE, params.encode(), response);
    if (!response) {
        result = std::unexpected(response.error());
        co_return;
    }

    auto initExp = parse_initialize_result(response.value());
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

galay::kernel::Task<void> HttpClientTransport::call_tool(std::string toolName,
                                        std::string arguments,
                                        std::expected<std::string, McpError>& result) {
    if (!m_initialized) {
        result = std::unexpected(McpError::not_initialized());
        co_return;
    }

    ToolCallParams params;
    params.name = std::move(toolName);
    params.arguments = arguments.empty() ? empty_object_string() : std::move(arguments);

    std::expected<std::string, McpError> response;
    co_await send_request(Methods::TOOLS_CALL, params.encode(), response);
    if (!response) {
        result = std::unexpected(response.error());
        co_return;
    }

    result = parse_tool_call_result(response.value());
    co_return;
}

galay::kernel::Task<void> HttpClientTransport::list_tools(std::expected<std::vector<Tool>, McpError>& result) {
    if (!m_initialized) {
        result = std::unexpected(McpError::not_initialized());
        co_return;
    }

    std::expected<std::string, McpError> response;
    co_await send_request(Methods::TOOLS_LIST, empty_object_string(), response);
    if (!response) {
        result = std::unexpected(response.error());
        co_return;
    }

    result = parse_list_field<Tool>(
        response.value(),
        "tools");
    co_return;
}

galay::kernel::Task<void> HttpClientTransport::list_resources(std::expected<std::vector<Resource>, McpError>& result) {
    if (!m_initialized) {
        result = std::unexpected(McpError::not_initialized());
        co_return;
    }

    std::expected<std::string, McpError> response;
    co_await send_request(Methods::RESOURCES_LIST, empty_object_string(), response);
    if (!response) {
        result = std::unexpected(response.error());
        co_return;
    }

    result = parse_list_field<Resource>(
        response.value(),
        "resources");
    co_return;
}

galay::kernel::Task<void> HttpClientTransport::read_resource(std::string uri,
                                            std::expected<std::string, McpError>& result) {
    if (!m_initialized) {
        result = std::unexpected(McpError::not_initialized());
        co_return;
    }

    auto params = json::serialize(ResourceParams{std::move(uri)});
    if (!params) {
        result = std::unexpected(McpError::invalid_message(params.error()));
        co_return;
    }

    std::expected<std::string, McpError> response;
    co_await send_request(Methods::RESOURCES_READ, std::move(*params), response);
    if (!response) {
        result = std::unexpected(response.error());
        co_return;
    }

    result = parse_first_text_content(response.value(), "contents");
    co_return;
}

galay::kernel::Task<void> HttpClientTransport::list_prompts(std::expected<std::vector<Prompt>, McpError>& result) {
    if (!m_initialized) {
        result = std::unexpected(McpError::not_initialized());
        co_return;
    }

    std::expected<std::string, McpError> response;
    co_await send_request(Methods::PROMPTS_LIST, empty_object_string(), response);
    if (!response) {
        result = std::unexpected(response.error());
        co_return;
    }

    result = parse_list_field<Prompt>(
        response.value(),
        "prompts");
    co_return;
}

galay::kernel::Task<void> HttpClientTransport::get_prompt(std::string name,
                                         std::string arguments,
                                         std::expected<std::string, McpError>& result) {
    if (!m_initialized) {
        result = std::unexpected(McpError::not_initialized());
        co_return;
    }

    auto params = json::serialize(PromptParams{std::move(name), arguments.empty() ?
        std::nullopt : std::optional<std::string>{std::move(arguments)}});
    if (!params) {
        result = std::unexpected(McpError::invalid_message(params.error()));
        co_return;
    }

    std::expected<std::string, McpError> response;
    co_await send_request(Methods::PROMPTS_GET, std::move(*params), response);
    if (!response) {
        result = std::unexpected(response.error());
        co_return;
    }

    result = response.value();
    co_return;
}

galay::kernel::Task<void> HttpClientTransport::ping(std::expected<void, McpError>& result) {
    if (!m_initialized) {
        result = std::unexpected(McpError::not_initialized());
        co_return;
    }

    std::expected<std::string, McpError> response;
    co_await send_request(Methods::PING, empty_object_string(), response);
    if (!response) {
        result = std::unexpected(response.error());
        co_return;
    }

    result = {};
    co_return;
}

bool HttpClientTransport::is_connected() const {
    return m_connected;
}

bool HttpClientTransport::is_initialized() const {
    return m_initialized;
}

const ServerInfo& HttpClientTransport::get_server_info() const {
    return m_serverInfo;
}

const ServerCapabilities& HttpClientTransport::get_server_capabilities() const {
    return m_serverCapabilities;
}

galay::kernel::Task<void> HttpClientTransport::send_request(std::string_view method,
                                           std::optional<std::string> params,
                                           std::expected<std::string, McpError>& result) {
    const int64_t requestId = generate_request_id();
    const std::optional<std::string_view> params_view =
        params.has_value() ? std::optional<std::string_view>(*params) : std::nullopt;
    std::string requestBody = protocol::make_json_rpc_request_body(requestId, method, params_view);

    if (!m_connected.load()) {
        auto connectResult = co_await m_httpClient->connect(m_serverUrl);
        if (!connectResult) {
            MCP_LOG_ERROR("[http_client]", "connect failed url={} error={}",
                          m_serverUrl,
                          connectResult.error().message());
            result = std::unexpected(McpError::connection_error(
                std::string(connectResult.error().message())));
            co_return;
        }
        MCP_LOG_INFO("[http_client]", "connected url={}", m_serverUrl);
        m_connected = true;
    }

    auto sessionResult = m_httpClient->get_session();
    if (!sessionResult) {
        m_connected = false;
        MCP_LOG_ERROR("[http_client]", "session create failed method={} id={} error={}",
                      method,
                      requestId,
                      sessionResult.error().message());
        result = std::unexpected(McpError::connection_error(
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
            result = std::unexpected(McpError::connection_error(
                std::string(httpResult.error().message())));
            co_return;
        }

        if (!httpResult.value()) {
            continue;
        }

        auto response = std::move(httpResult.value().value());
        if (response.header().is_connection_close() || !response.header().is_keep_alive()) {
            m_connected = false;
        }

        if (response.header().code() != http::HttpStatusCode::OK_200) {
            MCP_LOG_WARN("[http_client]", "unexpected http status method={} id={} status={}",
                         method,
                         requestId,
                         static_cast<int>(response.header().code()));
            result = std::unexpected(McpError::connection_error(
                "HTTP error: " + std::to_string(static_cast<int>(response.header().code()))));
            co_return;
        }

        std::string responseBody = response.get_body_str();
        auto parsed = parse_json_rpc_response(responseBody);
        if (!parsed) {
            MCP_LOG_WARN("[http_client]", "json-rpc response parse failed method={} id={} error={}",
                         method,
                         requestId,
                         parsed.error().details());
            result = std::unexpected(McpError::parse_error(parsed.error().details()));
            co_return;
        }

        const auto& view = parsed.value().response;
        if (view.id != requestId) {
            MCP_LOG_WARN("[http_client]", "mismatched response id expected={} actual={}", requestId, view.id);
            result = std::unexpected(McpError::invalid_response("Mismatched response id"));
            co_return;
        }
        if (view.hasError) {
            auto errorExp = JsonRpcError::decode(view.error);
            if (!errorExp) {
                MCP_LOG_WARN("[http_client]", "json-rpc error parse failed method={} id={} error={}",
                             method,
                             requestId,
                             errorExp.error().message());
                result = std::unexpected(McpError::parse_error(errorExp.error().message()));
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
            result = std::unexpected(McpError::from_json_rpc_error(
                error.code, error.message, details));
            co_return;
        }

        if (view.hasResult) {
            auto serialized = json::serialize(view.result);
            if (serialized) {
                result = std::move(*serialized);
            } else {
                MCP_LOG_WARN("[http_client]", "result serialization failed method={} id={}", method, requestId);
                result = std::unexpected(McpError::parse_error("Failed to parse result"));
            }
        } else {
            result = empty_object_string();
        }

        co_return;
    }
}

int64_t HttpClientTransport::generate_request_id() {
    return m_requestIdCounter.fetch_add(1, std::memory_order_relaxed) + 1;
}

} // namespace galay::mcp::detail
