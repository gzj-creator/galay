#include "client.h"

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

std::expected<std::string, McpError> required_string(const json::Json& object, const char* key)
{
    auto value = object.at(key).as_string();
    if (!value) {
        return std::unexpected(McpError::invalid_params(
            std::string("missing or invalid ") + key));
    }
    return std::string(*value);
}

template <typename T>
std::expected<std::vector<T>, McpError> parse_items(std::string_view result,
                                                   const char* key,
                                                   auto parser)
{
    auto document = JsonDocument::parse(result);
    if (!document) return std::unexpected(document.error());
    if (!document->root().is_object()) {
        return std::unexpected(McpError::invalid_response("result must be an object"));
    }
    const json::Json array = document->root().at(key);
    if (!array.is_array()) {
        return std::unexpected(McpError::invalid_response(std::string("missing ") + key));
    }
    std::vector<T> values;
    for (size_t i = 0; i < array.size(); ++i) {
        const json::Json item = array.at(i);
        auto value = parser(item);
        if (!value) return std::unexpected(value.error());
        values.push_back(std::move(value.value()));
    }
    return values;
}

std::expected<std::string, McpError> first_text(std::string_view result)
{
    auto document = JsonDocument::parse(result);
    if (!document) return std::unexpected(document.error());
    if (!document->root().is_object()) {
        return std::unexpected(McpError::invalid_response("result must be an object"));
    }
    const json::Json contents = document->root().at("contents");
    if (!contents.is_array()) {
        return std::unexpected(McpError::invalid_response("missing contents"));
    }
    for (size_t i = 0; i < contents.size(); ++i) {
        const json::Json item = contents.at(i);
        if (!item.is_object()) continue;
        auto text = item.at("text").as_string();
        if (text) return std::string(*text);
    }
    return std::string{};
}

std::expected<std::string, McpError> parse_rpc_result(std::string_view body,
                                                    RequestId expectedId)
{
    auto parsed = parse_response(body);
    if (!parsed) return std::unexpected(parsed.error());
    if (!parsed->response.hasResult) {
        if (!parsed->response.error.is_object()) {
            return std::unexpected(McpError::invalid_response("invalid error response"));
        }
        const auto code = parsed->response.error.at("code").as_int64();
        const auto message = parsed->response.error.at("message").as_string();
        if (!code || !message) {
            return std::unexpected(McpError::invalid_response("invalid error response"));
        }
        return std::unexpected(McpError::from_json_rpc_error(static_cast<int>(*code),
                                                          std::string(*message)));
    }
    if (parsed->response.id != expectedId) {
        return std::unexpected(McpError::invalid_response("mismatched response id"));
    }
    std::string result;
    auto serialized = json::stream::serialize(
        parsed->response.result, [&](std::string_view chunk) -> json::result<void> {
            result.append(chunk);
            return {};
        });
    if (!serialized) {
        return std::unexpected(McpError::invalid_response("invalid result"));
    }
    return result;
}

std::expected<SubscriptionFilter, McpError> parse_acknowledged(
    std::string_view message, const RequestId& requestId)
{
    auto document = JsonDocument::parse(message);
    if (!document) return std::unexpected(document.error());
    const json::Json object = document->root();
    if (!object.is_object()) {
        return std::unexpected(McpError::invalid_response("SSE message is not an object"));
    }
    const auto method = object.at("method").as_string();
    if (!method || *method != NotificationMethods::SUBSCRIPTIONS_ACKNOWLEDGED) {
        return std::unexpected(McpError::invalid_response("subscription acknowledgement is not first"));
    }
    const json::Json params = object.at("params");
    if (!params.is_object()) {
        return std::unexpected(McpError::invalid_response("acknowledgement missing params"));
    }
    const json::Json meta = params.at("_meta");
    const json::Json idElement = meta.is_object()
        ? meta.at("io.modelcontextprotocol/subscriptionId")
        : json::Json{};
    if (!meta.is_object() || !idElement.valid()) {
        return std::unexpected(McpError::invalid_response("acknowledgement missing subscription id"));
    }
    RequestId id;
    if (auto number = idElement.as_int64()) {
        id = *number;
    } else if (auto text = idElement.as_string()) {
        id = std::string(*text);
    } else {
        return std::unexpected(McpError::invalid_response("invalid subscription id"));
    }
    if (id != requestId) {
        return std::unexpected(McpError::invalid_response("mismatched subscription id"));
    }
    const json::Json filterElement = params.at("notifications");
    if (!filterElement.valid()) {
        return std::unexpected(McpError::invalid_response("acknowledgement missing notifications"));
    }
    return SubscriptionFilter::from_json(filterElement);
}

std::expected<bool, McpError> validate_subscription_message(
    std::string_view message, const RequestId& requestId)
{
    auto document = JsonDocument::parse(message);
    if (!document) return std::unexpected(document.error());
    const json::Json object = document->root();
    if (!object.is_object()) {
        return std::unexpected(McpError::invalid_response("SSE message is not an object"));
    }
    const auto method = object.at("method").as_string();
    if (method) {
        if (*method == NotificationMethods::SUBSCRIPTIONS_ACKNOWLEDGED) {
            return std::unexpected(McpError::invalid_response("duplicate subscription acknowledgement"));
        }
        if (*method != NotificationMethods::TOOLS_LIST_CHANGED &&
            *method != NotificationMethods::RESOURCES_LIST_CHANGED &&
            *method != NotificationMethods::RESOURCES_UPDATED &&
            *method != NotificationMethods::PROMPTS_LIST_CHANGED) {
            return std::unexpected(McpError::invalid_response("unknown subscription notification"));
        }
        const json::Json params = object.at("params");
        if (!params.is_object()) {
            return std::unexpected(McpError::invalid_response(
                "subscription notification missing subscription id"));
        }
        const json::Json meta = params.at("_meta");
        if (!meta.is_object()) {
            return std::unexpected(McpError::invalid_response(
                "subscription notification missing subscription id"));
        }
        const json::Json subscriptionElement =
            meta.at("io.modelcontextprotocol/subscriptionId");
        if (!subscriptionElement.valid()) {
            return std::unexpected(McpError::invalid_response(
                "subscription notification missing subscription id"));
        }
        RequestId notificationId;
        if (auto number = subscriptionElement.as_int64()) {
            notificationId = *number;
        } else if (auto text = subscriptionElement.as_string()) {
            notificationId = std::string(*text);
        } else {
            return std::unexpected(McpError::invalid_response(
                "invalid subscription notification id"));
        }
        if (notificationId != requestId) {
            return std::unexpected(McpError::invalid_response(
                "mismatched subscription notification id"));
        }
        return true;
    }
    auto parsed = parse_response(message);
    if (!parsed || !parsed->response.hasResult || parsed->response.id != requestId) {
        return std::unexpected(McpError::invalid_response("invalid subscription completion"));
    }
    if (!parsed->response.result.is_object()) {
        return std::unexpected(McpError::invalid_response("invalid subscription result"));
    }
    const auto resultType = parsed->response.result.at("resultType").as_string();
    if (!resultType || *resultType != "complete") {
        return std::unexpected(McpError::invalid_response("unexpected subscription result"));
    }
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
    auto writeResult = write(request.to_json());
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
    auto document = JsonDocument::parse(value.value());
    if (!document) return std::unexpected(document.error());
    return DiscoverResult::from_json(document->root());
}

std::expected<std::vector<Tool>, McpError> McpStdioClient::list_tools()
{
    auto value = request(Methods::TOOLS_LIST);
    if (!value) return std::unexpected(value.error());
    return parse_items<Tool>(value.value(), "tools", [](const json::Json& item) { return Tool::from_json(item); });
}

std::expected<std::string, McpError> McpStdioClient::call_tool(std::string name, std::string arguments)
{
    std::string fieldsJson;
    auto fields = make_json_writer(fieldsJson);
    // StreamWriter 失败粘滞：中间结果统一丢弃，由 finish() 统一检查
    (void)fields.start_object();
    (void)fields.key("name");
    (void)fields.string(name);
    (void)fields.key("arguments");
    (void)fields.raw(arguments.empty() ? "{}" : arguments);
    (void)fields.end_object();
    auto finished = fields.finish();
    if (!finished) {
        return std::unexpected(McpError::invalid_message(
            "failed to encode JSON: " + finished.error()));
    }
    auto value = request(Methods::TOOLS_CALL, std::move(fieldsJson));
    if (!value) return std::unexpected(value.error());
    return value;
}

std::expected<std::vector<Resource>, McpError> McpStdioClient::list_resources()
{
    auto value = request(Methods::RESOURCES_LIST);
    if (!value) return std::unexpected(value.error());
    return parse_items<Resource>(value.value(), "resources", [](const json::Json& item) { return Resource::from_json(item); });
}

std::expected<std::string, McpError> McpStdioClient::read_resource(std::string uri)
{
    std::string fieldsJson;
    auto fields = make_json_writer(fieldsJson);
    // StreamWriter 失败粘滞：中间结果统一丢弃，由 finish() 统一检查
    (void)fields.start_object();
    (void)fields.key("uri");
    (void)fields.string(uri);
    (void)fields.end_object();
    auto finished = fields.finish();
    if (!finished) {
        return std::unexpected(McpError::invalid_message(
            "failed to encode JSON: " + finished.error()));
    }
    auto value = request(Methods::RESOURCES_READ, std::move(fieldsJson));
    if (!value) return std::unexpected(value.error());
    return first_text(value.value());
}

std::expected<std::vector<Prompt>, McpError> McpStdioClient::list_prompts()
{
    auto value = request(Methods::PROMPTS_LIST);
    if (!value) return std::unexpected(value.error());
    return parse_items<Prompt>(value.value(), "prompts", [](const json::Json& item) { return Prompt::from_json(item); });
}

std::expected<std::string, McpError> McpStdioClient::get_prompt(std::string name, std::string arguments)
{
    std::string fieldsJson;
    auto fields = make_json_writer(fieldsJson);
    // StreamWriter 失败粘滞：中间结果统一丢弃，由 finish() 统一检查
    (void)fields.start_object();
    (void)fields.key("name");
    (void)fields.string(name);
    (void)fields.key("arguments");
    (void)fields.raw(arguments.empty() ? "{}" : arguments);
    (void)fields.end_object();
    auto finished = fields.finish();
    if (!finished) {
        return std::unexpected(McpError::invalid_message(
            "failed to encode JSON: " + finished.error()));
    }
    return request(Methods::PROMPTS_GET, std::move(fieldsJson));
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
    const std::string body = message.to_json();
    auto session = m_client.get_session();
    if (!session) { result = std::unexpected(McpError::connection_error(session.error().message())); co_return; }
    std::string nameHeader;
    if (method == Methods::TOOLS_CALL || method == Methods::PROMPTS_GET) {
        auto doc = JsonDocument::parse(fields);
        if (doc && doc->root().is_object()) {
            if (auto name = doc->root().at("name").as_string()) {
                nameHeader = std::string(*name);
            }
        }
    } else if (method == Methods::RESOURCES_READ) {
        auto doc = JsonDocument::parse(fields);
        if (doc && doc->root().is_object()) {
            if (auto uri = doc->root().at("uri").as_string()) {
                nameHeader = std::string(*uri);
            }
        }
    }
    std::map<std::string, std::string> headers{
        {"Host", m_client.url().host + ":" + std::to_string(m_client.url().port)},
        {"Accept", "application/json, text/event-stream"},
        {"MCP-Protocol-Version", MCP_VERSION},
        {"Mcp-Method", method}};
    if (!nameHeader.empty()) headers.emplace("Mcp-Name", encode_header_value(nameHeader));
    if (method == Methods::TOOLS_CALL) {
        auto fieldsDocument = JsonDocument::parse(fields);
        if (fieldsDocument && fieldsDocument->root().is_object()) {
            const json::Json& fieldsObject = fieldsDocument->root();
            const auto toolName = fieldsObject.at("name").as_string();
            const json::Json arguments = fieldsObject.at("arguments");
            if (toolName && arguments.valid()) {
                auto it = m_toolDefinitions.find(std::string(*toolName));
                if (it != m_toolDefinitions.end()) {
                    auto annotations = tool_header_annotations(it->second);
                    if (!annotations) {
                        result = std::unexpected(annotations.error());
                        co_return;
                    }
                    for (const auto& annotation : annotations.value()) {
                        auto value = argument_header_value(arguments, annotation);
                        if (!value) {
                            result = std::unexpected(value.error());
                            co_return;
                        }
                        if (value.value()) {
                            headers.emplace("Mcp-Param-" + annotation.name,
                                            encode_header_value(*value.value()));
                        }
                    }
                }
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
    auto doc = JsonDocument::parse(value.value());
    if (!doc) { result = std::unexpected(doc.error()); co_return; }
    result = DiscoverResult::from_json(doc->root());
}

kernel::Task<void> McpHttpClient::list_tools(std::expected<std::vector<Tool>, McpError>& result)
{
    std::expected<std::string, McpError> value;
    co_await request(Methods::TOOLS_LIST, "{}", value);
    if (!value) { result = std::unexpected(value.error()); co_return; }
    auto parsed = parse_items<Tool>(value.value(), "tools", [](const json::Json& item) { return Tool::from_json(item); });
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
    std::string fieldsJson;
    auto fields = make_json_writer(fieldsJson);
    // StreamWriter 失败粘滞：中间结果统一丢弃，由 finish() 统一检查
    (void)fields.start_object();
    (void)fields.key("name");
    (void)fields.string(name);
    (void)fields.key("arguments");
    (void)fields.raw(arguments.empty() ? "{}" : arguments);
    (void)fields.end_object();
    auto finished = fields.finish();
    if (!finished) {
        result = std::unexpected(McpError::invalid_message(
            "failed to encode JSON: " + finished.error()));
        co_return;
    }
    co_await request(Methods::TOOLS_CALL, std::move(fieldsJson), result);
}

kernel::Task<void> McpHttpClient::list_resources(std::expected<std::vector<Resource>, McpError>& result)
{
    std::expected<std::string, McpError> value;
    co_await request(Methods::RESOURCES_LIST, "{}", value);
    if (!value) { result = std::unexpected(value.error()); co_return; }
    result = parse_items<Resource>(value.value(), "resources", [](const json::Json& item) { return Resource::from_json(item); });
}

kernel::Task<void> McpHttpClient::read_resource(std::string uri,
                                               std::expected<std::string, McpError>& result)
{
    std::string fieldsJson;
    auto fields = make_json_writer(fieldsJson);
    // StreamWriter 失败粘滞：中间结果统一丢弃，由 finish() 统一检查
    (void)fields.start_object();
    (void)fields.key("uri");
    (void)fields.string(uri);
    (void)fields.end_object();
    auto finished = fields.finish();
    if (!finished) {
        result = std::unexpected(McpError::invalid_message(
            "failed to encode JSON: " + finished.error()));
        co_return;
    }
    std::expected<std::string, McpError> value;
    co_await request(Methods::RESOURCES_READ, std::move(fieldsJson), value);
    if (!value) { result = std::unexpected(value.error()); co_return; }
    result = first_text(value.value());
}

kernel::Task<void> McpHttpClient::list_prompts(std::expected<std::vector<Prompt>, McpError>& result)
{
    std::expected<std::string, McpError> value;
    co_await request(Methods::PROMPTS_LIST, "{}", value);
    if (!value) { result = std::unexpected(value.error()); co_return; }
    result = parse_items<Prompt>(value.value(), "prompts", [](const json::Json& item) { return Prompt::from_json(item); });
}

kernel::Task<void> McpHttpClient::get_prompt(std::string name, std::string arguments,
                                            std::expected<std::string, McpError>& result)
{
    std::string fieldsJson;
    auto fields = make_json_writer(fieldsJson);
    // StreamWriter 失败粘滞：中间结果统一丢弃，由 finish() 统一检查
    (void)fields.start_object();
    (void)fields.key("name");
    (void)fields.string(name);
    (void)fields.key("arguments");
    (void)fields.raw(arguments.empty() ? "{}" : arguments);
    (void)fields.end_object();
    auto finished = fields.finish();
    if (!finished) {
        result = std::unexpected(McpError::invalid_message(
            "failed to encode JSON: " + finished.error()));
        co_return;
    }
    co_await request(Methods::PROMPTS_GET, std::move(fieldsJson), result);
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
    auto params = make_request_params(meta(), [&filter] {
        std::string out;
        auto fields = make_json_writer(out);
        // StreamWriter 失败粘滞：中间结果统一丢弃，由 finish() 统一检查
        (void)fields.start_object();
        (void)fields.key("notifications");
        (void)fields.raw(filter.to_json());
        (void)fields.end_object();
        if (!fields.finish()) {
            return std::string{};
        }
        return out;
    }());
    if (!params) {
        result = std::unexpected(params.error());
        co_return;
    }
    JsonRpcRequest request;
    request.id = requestId;
    request.method = Methods::SUBSCRIPTIONS_LISTEN;
    request.params = std::move(params.value());

    std::string requestBody = request.to_json();
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
