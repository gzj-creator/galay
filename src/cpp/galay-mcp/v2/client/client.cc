#include "client.h"
#include "../../common/mcp_base.h"

#include "../../../galay-http/builder/http_builder.h"
#include "../../../galay-kernel/core/runtime.h"
#include "../common/http_headers.h"

#include <map>
#include <thread>

namespace galay::mcp::v2 {

namespace {

struct HttpRequestLease {
    explicit HttpRequestLease(bool& active) noexcept : flag(active) { flag = true; }
    ~HttpRequestLease() { flag = false; }
    HttpRequestLease(const HttpRequestLease&) = delete;
    HttpRequestLease& operator=(const HttpRequestLease&) = delete;
    bool& flag;
};

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

template <typename T>
std::expected<std::vector<T>, McpError> parse_items(std::string_view result, const char* key) {
    auto values = json::deserialize_member<std::vector<json::Json>>(result, key);
    if (!values) return std::unexpected(McpError::invalid_response(values.error()));
    std::vector<T> items;
    items.reserve(values->size());
    for (const auto& value : *values) {
        auto item = T::decode(value);
        if (!item) return std::unexpected(item.error());
        items.push_back(std::move(*item));
    }
    return items;
}
std::expected<std::string, McpError> first_text(std::string_view result) {
    auto values = json::deserialize_member<std::vector<TextResource>>(result, "contents");
    if (!values) return std::unexpected(McpError::invalid_response(values.error()));
    if (values->empty()) return std::string{};
    return std::move(values->front().text);
}
std::expected<std::string, McpError> parse_rpc_result(std::string_view body, RequestId expected_id) {
    auto parsed = parse_response(body);
    if (!parsed) return std::unexpected(parsed.error());
    if (parsed->response.hasError) {
        auto error = json::decode<galay::mcp::JsonRpcError>(parsed->response.error);
        if (!error) return std::unexpected(McpError::invalid_response(error.error()));
        return std::unexpected(McpError::from_json_rpc_error(error->code, error->message,
            error->data.value_or(std::string{})));
    }
    if (parsed->response.id != expected_id)
        return std::unexpected(McpError::invalid_response("mismatched response id"));
    auto result = json::serialize(parsed->response.result);
    if (!result) return std::unexpected(McpError::invalid_response(result.error()));
    return std::move(*result);
}
std::expected<SubscriptionFilter, McpError> parse_acknowledged(std::string_view message, const RequestId& request_id) {
    auto fields = json::deserialize<EnvelopeFields>(message);
    if (!fields) return std::unexpected(McpError::invalid_response(fields.error()));
    if (fields->method != NotificationMethods::SUBSCRIPTIONS_ACKNOWLEDGED || !fields->params)
        return std::unexpected(McpError::invalid_response("subscription acknowledgement is not first"));
    auto params = json::deserialize<SubscriptionParams>(*fields->params);
    if (!params) return std::unexpected(McpError::invalid_response(params.error()));
    if (params->meta.id != request_id || !params->notifications)
        return std::unexpected(McpError::invalid_response("invalid subscription acknowledgement"));
    return std::move(*params->notifications);
}
std::expected<bool, McpError> validate_subscription_message(std::string_view message, const RequestId& request_id) {
    auto fields = json::deserialize<EnvelopeFields>(message);
    if (!fields) return std::unexpected(McpError::invalid_response(fields.error()));
    if (fields->method) {
        const auto& method = *fields->method;
        if (method != NotificationMethods::TOOLS_LIST_CHANGED && method != NotificationMethods::RESOURCES_LIST_CHANGED &&
            method != NotificationMethods::RESOURCES_UPDATED && method != NotificationMethods::PROMPTS_LIST_CHANGED)
            return std::unexpected(McpError::invalid_response("unknown subscription notification"));
        if (!fields->params) return std::unexpected(McpError::invalid_response("missing subscription params"));
        auto params = json::deserialize<SubscriptionParams>(*fields->params);
        if (!params) return std::unexpected(McpError::invalid_response(params.error()));
        if (params->meta.id != request_id)
            return std::unexpected(McpError::invalid_response("mismatched subscription notification id"));
        return true;
    }
    auto parsed = parse_response(message);
    if (!parsed || !parsed->response.hasResult || parsed->response.id != request_id)
        return std::unexpected(McpError::invalid_response("invalid subscription completion"));
    auto type = json::decode_member<std::string>(parsed->response.result, "resultType");
    if (!type || *type != "complete") return std::unexpected(McpError::invalid_response("unexpected subscription result"));
    return false;
}

bool has_sse_content_type(http::HttpResponseHeader& header)
{
    auto value = header.header_pairs().get_value("Content-Type");
    if (value.empty()) value = header.header_pairs().get_value("content-type");
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

std::int64_t McpStdioClient::next_id() noexcept
{
    return m_nextId.fetch_add(1, std::memory_order_relaxed) + 1;
}

std::expected<void, McpError> McpStdioClient::write(std::string_view message)
{
    if (m_input == nullptr || m_output == nullptr) {
        return std::unexpected(McpError::invalid_params("stdio stream is null"));
    }
    (*m_output) << message << '\n' << std::flush;
    if (!*m_output) return std::unexpected(McpError::write_error("stdio write failed"));
    return {};
}

std::expected<std::string, McpError> McpStdioClient::read()
{
    std::string line;
    if (m_input == nullptr || !std::getline(*m_input, line)) {
        return std::unexpected(McpError::connection_closed("stdio input closed"));
    }
    return line;
}

std::expected<std::string, McpError> McpStdioClient::request(std::string_view method,
                                                            std::string fields)
{
    if (m_requestActive.test_and_set(std::memory_order_acquire)) {
        return std::unexpected(McpError::overload("stdio client request already active"));
    }
    StdioRequestLease lease{&m_requestActive};
    const RequestId id = next_id();
    auto params = make_request_params(meta(), fields);
    if (!params) return std::unexpected(params.error());
    JsonRpcRequest request;
    request.id = id;
    request.method = std::string(method);
    request.params = std::move(params.value());
    auto writeResult = write(request.encode());
    if (!writeResult) return std::unexpected(writeResult.error());
    while (true) {
        auto line = read();
        if (!line) return std::unexpected(line.error());
        auto response = parse_response(line.value());
        if (!response) return std::unexpected(response.error());
        if (response->response.id != id) continue;
        return parse_rpc_result(line.value(), id);
    }
}

std::expected<DiscoverResult, McpError> McpStdioClient::discover()
{
    auto value = request(Methods::SERVER_DISCOVER);
    if (!value) return std::unexpected(value.error());
    auto result = json::deserialize<DiscoverResult>(*value);
    if (!result) return std::unexpected(McpError::invalid_response(result.error()));
    return std::move(*result);
}

std::expected<std::vector<Tool>, McpError> McpStdioClient::list_tools()
{
    auto value = request(Methods::TOOLS_LIST);
    if (!value) return std::unexpected(value.error());
    return parse_items<Tool>(value.value(), "tools");
}

std::expected<std::string, McpError> McpStdioClient::call_tool(std::string name, std::string arguments)
{
    auto finished = json::serialize(galay::mcp::PromptParams{name, arguments.empty() ? std::string("{}") : arguments});
    if (!finished) {
        return std::unexpected(McpError::invalid_message(
            "failed to encode JSON: " + finished.error()));
    }
    auto value = request(Methods::TOOLS_CALL, std::move(*finished));
    if (!value) return std::unexpected(value.error());
    return value;
}

std::expected<std::vector<Resource>, McpError> McpStdioClient::list_resources()
{
    auto value = request(Methods::RESOURCES_LIST);
    if (!value) return std::unexpected(value.error());
    return parse_items<Resource>(value.value(), "resources");
}

std::expected<std::string, McpError> McpStdioClient::read_resource(std::string uri)
{
    auto finished = json::serialize(galay::mcp::ResourceParams{uri});
    if (!finished) {
        return std::unexpected(McpError::invalid_message(
            "failed to encode JSON: " + finished.error()));
    }
    auto value = request(Methods::RESOURCES_READ, std::move(*finished));
    if (!value) return std::unexpected(value.error());
    return first_text(value.value());
}

std::expected<std::vector<Prompt>, McpError> McpStdioClient::list_prompts()
{
    auto value = request(Methods::PROMPTS_LIST);
    if (!value) return std::unexpected(value.error());
    return parse_items<Prompt>(value.value(), "prompts");
}

std::expected<std::string, McpError> McpStdioClient::get_prompt(std::string name, std::string arguments)
{
    auto finished = json::serialize(galay::mcp::PromptParams{name, arguments.empty() ? std::string("{}") : arguments});
    if (!finished) {
        return std::unexpected(McpError::invalid_message(
            "failed to encode JSON: " + finished.error()));
    }
    return request(Methods::PROMPTS_GET, std::move(*finished));
}

McpHttpClient::McpHttpClient(kernel::Runtime& runtime,
                             std::string url,
                             ClientConfig config)
    : m_client(http::HttpClientBuilder().build())
    , m_config(std::move(config))
    , m_url(std::move(url))
    , m_owner(runtime.get_io_scheduler(0))
{
}

bool McpHttpClient::on_owner() const noexcept
{
    return m_owner != nullptr && m_owner->thread_id() == std::this_thread::get_id();
}

std::expected<McpHttpClient::ConnectAwaitable, McpError> McpHttpClient::connect()
{
    if (!on_owner()) return std::unexpected(McpError::invalid_params("HTTP client requires its owner scheduler"));
    if (m_requestActive) return std::unexpected(McpError::overload("HTTP request already active"));
    m_toolDefinitions.clear();
    return m_client.connect(m_url);
}

std::expected<McpHttpClient::CloseAwaitable, McpError> McpHttpClient::close()
{
    if (!on_owner()) return std::unexpected(McpError::invalid_params("HTTP client requires its owner scheduler"));
    if (m_requestActive) return std::unexpected(McpError::overload("HTTP request already active"));
    m_toolDefinitions.clear();
    return m_client.close();
}

RequestMeta McpHttpClient::meta() const
{
    RequestMeta value;
    value.clientCapabilities = m_config.clientCapabilities;
    value.clientInfo = Implementation{.name = m_config.clientName, .version = m_config.clientVersion};
    return value;
}

std::int64_t McpHttpClient::next_id() noexcept
{
    return ++m_nextId;
}

kernel::Task<void> McpHttpClient::request(std::string method,
                                          std::string fields,
                                          std::expected<std::string, McpError>& result)
{
    if (!on_owner()) {
        result = std::unexpected(McpError::invalid_params("HTTP client requires its owner scheduler"));
        co_return;
    }
    if (m_requestActive) {
        result = std::unexpected(McpError::overload("HTTP request already active"));
        co_return;
    }
    HttpRequestLease lease(m_requestActive);
    auto socket = m_client.socket();
    if (!socket || socket->get().handle() == GHandle::invalid()) {
        auto connected = co_await m_client.connect(m_url);
        if (!connected) {
            result = std::unexpected(McpError::connection_error(std::string(connected.error().message())));
            co_return;
        }
        if (!*connected) {
            result = std::unexpected(McpError::connection_error(std::string(connected->error().message())));
            co_return;
        }
    }
    const RequestId id = next_id();
    auto params = make_request_params(meta(), fields);
    if (!params) { result = std::unexpected(params.error()); co_return; }
    JsonRpcRequest message;
    message.id = id;
    message.method = method;
    message.params = std::move(params.value());
    const std::string body = message.encode();
    auto session = m_client.get_session();
    if (!session) { result = std::unexpected(McpError::connection_error(session.error().message())); co_return; }
    std::string nameHeader;
    if (method == Methods::TOOLS_CALL || method == Methods::PROMPTS_GET || method == Methods::RESOURCES_READ) {
        const char* key = method == Methods::RESOURCES_READ ? "uri" : "name";
        auto name = json::deserialize_member<std::string>(fields, key);
        if (!name) { result = std::unexpected(McpError::invalid_params(name.error())); co_return; }
        nameHeader = std::move(*name);
    }
    std::map<std::string, std::string> headers{
        {"Host", m_client.url().host + ":" + std::to_string(m_client.url().port)},
        {"Accept", "application/json, text/event-stream"},
        {"MCP-Protocol-Version", MCP_VERSION},
        {"Mcp-Method", method}};
    if (!nameHeader.empty()) headers.emplace("Mcp-Name", encode_header_value(nameHeader));
    if (method == Methods::TOOLS_CALL) {
        auto named = json::deserialize<galay::mcp::NamedArguments>(fields);
        if (!named) { result = std::unexpected(McpError::invalid_params(named.error())); co_return; }
        auto it = m_toolDefinitions.find(named->name);
        if (it != m_toolDefinitions.end()) {
            auto annotations = tool_header_annotations(it->second);
            if (!annotations) { result = std::unexpected(annotations.error()); co_return; }
            for (const auto& annotation : *annotations) {
                auto value = argument_header_value(named->arguments, annotation);
                if (!value) { result = std::unexpected(value.error()); co_return; }
                if (*value) headers.emplace("Mcp-Param-" + annotation.name, encode_header_value(**value));
            }
        }
    }
    auto response = session.value()->post(m_client.url().path, body, "application/json", headers);
    while (true) {
        auto received = co_await response;
        if (!received) { result = std::unexpected(McpError::connection_error(std::string(received.error().message()))); co_return; }
        if (!received.value()) continue;
        auto value = std::move(received.value().value());
        if (value.header().code() != http::HttpStatusCode::OK_200) {
            auto rpcError = parse_rpc_result(value.get_body_str(), id);
            if (!rpcError) {
                result = std::unexpected(rpcError.error());
            } else {
                result = std::unexpected(McpError::connection_error(
                    "HTTP error: " + std::to_string(static_cast<int>(value.header().code()))));
            }
            co_return;
        }
        result = parse_rpc_result(value.get_body_str(), id);
        co_return;
    }
}

kernel::Task<void> McpHttpClient::discover(std::expected<DiscoverResult, McpError>& result)
{
    std::expected<std::string, McpError> value;
    co_await request(Methods::SERVER_DISCOVER, "{}", value);
    if (!value) { result = std::unexpected(value.error()); co_return; }
    result = json::deserialize<DiscoverResult>(*value)
        .transform_error([](const std::string& message) { return McpError::invalid_response(message); });
}

kernel::Task<void> McpHttpClient::list_tools(std::expected<std::vector<Tool>, McpError>& result)
{
    std::expected<std::string, McpError> value;
    co_await request(Methods::TOOLS_LIST, "{}", value);
    if (!value) { result = std::unexpected(value.error()); co_return; }
    auto parsed = parse_items<Tool>(value.value(), "tools");
    if (!parsed) { result = std::unexpected(parsed.error()); co_return; }
    std::vector<Tool> valid;
    for (auto& tool : parsed.value()) {
        auto annotations = tool_header_annotations(tool);
        if (!annotations) continue;
        valid.push_back(std::move(tool));
    }
    m_toolDefinitions.clear();
    m_toolDefinitions.reserve(valid.size());
    for (const auto& tool : valid) m_toolDefinitions.insert_or_assign(tool.name, tool);
    result = std::move(valid);
}

kernel::Task<void> McpHttpClient::call_tool(std::string name, std::string arguments,
                                           std::expected<std::string, McpError>& result)
{
    auto finished = json::serialize(galay::mcp::PromptParams{name, arguments.empty() ? std::string("{}") : arguments});
    if (!finished) {
        result = std::unexpected(McpError::invalid_message(
            "failed to encode JSON: " + finished.error()));
        co_return;
    }
    co_await request(Methods::TOOLS_CALL, std::move(*finished), result);
}

kernel::Task<void> McpHttpClient::list_resources(std::expected<std::vector<Resource>, McpError>& result)
{
    std::expected<std::string, McpError> value;
    co_await request(Methods::RESOURCES_LIST, "{}", value);
    if (!value) { result = std::unexpected(value.error()); co_return; }
    result = parse_items<Resource>(value.value(), "resources");
}

kernel::Task<void> McpHttpClient::read_resource(std::string uri,
                                               std::expected<std::string, McpError>& result)
{
    auto finished = json::serialize(galay::mcp::ResourceParams{uri});
    if (!finished) {
        result = std::unexpected(McpError::invalid_message(
            "failed to encode JSON: " + finished.error()));
        co_return;
    }
    std::expected<std::string, McpError> value;
    co_await request(Methods::RESOURCES_READ, std::move(*finished), value);
    if (!value) { result = std::unexpected(value.error()); co_return; }
    result = first_text(value.value());
}

kernel::Task<void> McpHttpClient::list_prompts(std::expected<std::vector<Prompt>, McpError>& result)
{
    std::expected<std::string, McpError> value;
    co_await request(Methods::PROMPTS_LIST, "{}", value);
    if (!value) { result = std::unexpected(value.error()); co_return; }
    result = parse_items<Prompt>(value.value(), "prompts");
}

kernel::Task<void> McpHttpClient::get_prompt(std::string name, std::string arguments,
                                            std::expected<std::string, McpError>& result)
{
    auto finished = json::serialize(galay::mcp::PromptParams{name, arguments.empty() ? std::string("{}") : arguments});
    if (!finished) {
        result = std::unexpected(McpError::invalid_message(
            "failed to encode JSON: " + finished.error()));
        co_return;
    }
    co_await request(Methods::PROMPTS_GET, std::move(*finished), result);
}

kernel::Task<void> McpHttpClient::listen(
    SubscriptionFilter filter,
    SubscriptionCallback callback,
    std::expected<SubscriptionFilter, McpError>& result)
{
    if (!on_owner()) {
        result = std::unexpected(McpError::invalid_params("HTTP client requires its owner scheduler"));
        co_return;
    }
    if (!callback) {
        result = std::unexpected(McpError::invalid_params("subscription callback is empty"));
        co_return;
    }

    http::HttpClient listenClient(http::HttpClientBuilder().build());
    auto connected = co_await listenClient.connect(m_url);
    if (!connected) {
        result = std::unexpected(McpError::connection_error(
            std::string(connected.error().message())));
        co_return;
    }
    if (!*connected) {
        result = std::unexpected(McpError::connection_error(
            std::string(connected->error().message())));
        co_return;
    }
    auto session = listenClient.get_session(64 * 1024);
    if (!session) {
        result = std::unexpected(McpError::connection_error(session.error().message()));
        co_return;
    }

    const RequestId requestId = next_id();
    auto fields = json::serialize(std::map<std::string, SubscriptionFilter>{{"notifications", filter}});
    if (!fields) { result = std::unexpected(McpError::invalid_params(fields.error())); co_return; }
    auto params = make_request_params(meta(), *fields);
    if (!params) {
        result = std::unexpected(params.error());
        co_return;
    }
    JsonRpcRequest request;
    request.id = requestId;
    request.method = Methods::SUBSCRIPTIONS_LISTEN;
    request.params = std::move(params.value());

    std::string requestBody = request.encode();
    http::HttpRequest httpRequest;
    http::HttpRequestHeader httpHeader;
    httpHeader.method() = http::HttpMethod::POST;
    httpHeader.uri() = listenClient.url().path;
    httpHeader.version() = http::HttpVersion::HttpVersion_1_1;
    httpHeader.header_pairs().add_header_pair("Host", listenClient.url().host + ":" +
                                           std::to_string(listenClient.url().port));
    httpHeader.header_pairs().add_header_pair("Accept", "application/json, text/event-stream");
    httpHeader.header_pairs().add_header_pair("Content-Type", "application/json");
    httpHeader.header_pairs().add_header_pair("Connection", "close");
    httpHeader.header_pairs().add_header_pair("MCP-Protocol-Version", MCP_VERSION);
    httpHeader.header_pairs().add_header_pair("Mcp-Method", Methods::SUBSCRIPTIONS_LISTEN);
    httpHeader.header_pairs().add_header_pair("Content-Length", std::to_string(requestBody.size()));
    httpRequest.set_header(std::move(httpHeader));
    httpRequest.set_body_str(std::move(requestBody));

    auto writer = session.value()->get_writer();
    auto sent = co_await writer.send_request(httpRequest);
    if (!sent || !sent.value()) {
        result = std::unexpected(McpError::connection_error(
            sent ? "failed to send subscription request" : sent.error().message()));
        co_return;
    }
    http::HttpResponseHeader response_header;
    auto headerResult = co_await session.value()->get_response_header(response_header);
    if (!headerResult || !headerResult.value() ||
        response_header.code() != http::HttpStatusCode::OK_200 ||
        !has_sse_content_type(response_header)) {
        result = std::unexpected(McpError::invalid_response("invalid subscription SSE response"));
        co_return;
    }

    http::ChunkParser chunkParser;
    std::string chunk;
    std::string sseBuffer;
    bool acknowledged = false;
    bool completed = false;
    while (!completed) {
        chunk.clear();
        auto chunkResult = co_await session.value()->get_next_chunk(chunk, chunkParser);
        if (!chunkResult) {
            result = std::unexpected(McpError::connection_closed(chunkResult.error().message()));
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
            auto parsedEvent = parse_sse_event(event);
            if (!parsedEvent) {
                result = std::unexpected(parsedEvent.error());
                co_return;
            }
            if (!parsedEvent->has_value()) continue;
            const auto message = std::move(parsedEvent->value());
            if (!acknowledged) {
                auto accepted = parse_acknowledged(message, requestId);
                if (!accepted) {
                    result = std::unexpected(accepted.error());
                    co_return;
                }
                result = std::move(accepted.value());
                acknowledged = true;
                continue;
            }
            auto messageState = validate_subscription_message(message, requestId);
            if (!messageState) {
                result = std::unexpected(messageState.error());
                co_return;
            }
            completed = !messageState.value();
            if (!completed && !callback(std::string(message))) {
                auto closed = co_await listenClient.close();
                if (!closed) {
                    result = std::unexpected(McpError::connection_error(
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
