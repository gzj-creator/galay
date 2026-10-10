#include "protocol.h"
#include "../../common/mcp_base.h"

namespace galay::mcp::v2 {

std::string Implementation::encode() const {
    auto value = json::serialize(*this);
    if (!value) return {};
    return std::move(*value);
}
std::expected<Implementation, McpError> Implementation::decode(const json::Json& element) {
    auto value = json::decode<Implementation>(element);
    if (!value) return std::unexpected(McpError::invalid_params(value.error()));
    return std::move(*value);
}
std::string RequestMeta::encode() const {
    auto value = json::serialize(*this);
    if (!value) return {};
    return std::move(*value);
}
std::expected<RequestMeta, McpError> RequestMeta::decode(const json::Json& element) {
    auto value = json::decode<RequestMeta>(element);
    if (!value) return std::unexpected(McpError::invalid_params(value.error()));
    return std::move(*value);
}
std::string ServerCapabilities::encode() const {
    auto value = json::serialize(*this);
    if (!value) return {};
    return std::move(*value);
}
std::expected<ServerCapabilities, McpError> ServerCapabilities::decode(const json::Json& element) {
    auto value = json::decode<ServerCapabilities>(element);
    if (!value) return std::unexpected(McpError::invalid_params(value.error()));
    return std::move(*value);
}
std::string Tool::encode() const {
    auto value = json::serialize(*this);
    if (!value) return {};
    return std::move(*value);
}
std::expected<Tool, McpError> Tool::decode(const json::Json& element) {
    auto value = json::decode<Tool>(element);
    if (!value) return std::unexpected(McpError::invalid_params(value.error()));
    auto type = json::deserialize_member<std::string>(value->inputSchema, "type");
    if (!type || *type != "object")
        return std::unexpected(McpError::invalid_params("tool inputSchema root type must be object"));
    return std::move(*value);
}
std::string Resource::encode() const {
    auto value = json::serialize(*this);
    if (!value) return {};
    return std::move(*value);
}
std::expected<Resource, McpError> Resource::decode(const json::Json& element) {
    auto value = json::decode<Resource>(element);
    if (!value) return std::unexpected(McpError::invalid_params(value.error()));
    return std::move(*value);
}
std::string PromptArgument::encode() const {
    auto value = json::serialize(*this);
    if (!value) return {};
    return std::move(*value);
}
std::expected<PromptArgument, McpError> PromptArgument::decode(const json::Json& element) {
    auto value = json::decode<PromptArgument>(element);
    if (!value) return std::unexpected(McpError::invalid_params(value.error()));
    return std::move(*value);
}
std::string Prompt::encode() const {
    auto value = json::serialize(*this);
    if (!value) return {};
    return std::move(*value);
}
std::expected<Prompt, McpError> Prompt::decode(const json::Json& element) {
    auto value = json::decode<Prompt>(element);
    if (!value) return std::unexpected(McpError::invalid_params(value.error()));
    return std::move(*value);
}
std::string DiscoverResult::encode() const {
    auto value = json::serialize(*this);
    if (!value) return {};
    return std::move(*value);
}
std::expected<DiscoverResult, McpError> DiscoverResult::decode(const json::Json& element) {
    auto value = json::decode<DiscoverResult>(element);
    if (!value) return std::unexpected(McpError::invalid_params(value.error()));
    return std::move(*value);
}
std::string ListResult::encode() const {
    std::vector<json::RawValue> raw_items;
    for (const auto& item : items) raw_items.push_back({item});
    auto list = json::serialize(raw_items);
    auto ttl = json::serialize(ttlMs);
    auto scope = json::serialize(cacheScope == CacheScope::Public ? "public" : "private");
    if (!list || !ttl || !scope) return {};
    json::Object fields{{"resultType", {"\"complete\""}}, {field, {std::move(*list)}},
        {"ttlMs", {std::move(*ttl)}}, {"cacheScope", {std::move(*scope)}}};
    if (nextCursor) {
        auto cursor = json::serialize(*nextCursor);
        if (!cursor) return {};
        fields.emplace("nextCursor", json::RawValue{std::move(*cursor)});
    }
    if (serverInfo) {
        auto meta = json::serialize(ServerMeta{serverInfo});
        if (!meta) return {};
        fields.emplace("_meta", json::RawValue{std::move(*meta)});
    }
    auto value = json::serialize(fields);
    if (!value) return {};
    return std::move(*value);
}
std::string JsonRpcRequest::encode() const {
    auto value = json::serialize(*this);
    if (!value) return {};
    return std::move(*value);
}
std::string make_request_params(const RequestMeta& meta) {
    auto result = make_request_params(meta, "{}");
    if (!result) return {};
    return std::move(*result);
}
std::expected<std::string, McpError> make_request_params(const RequestMeta& meta, std::string_view fields) {
    auto values = json::deserialize<json::Object>(fields);
    if (!values) return std::unexpected(McpError::invalid_params(values.error()));
    if (values->contains("_meta"))
        return std::unexpected(McpError::invalid_params("request fields must not contain _meta"));
    auto encoded_meta = json::serialize(meta);
    if (!encoded_meta) return std::unexpected(McpError::invalid_params(encoded_meta.error()));
    if (!values->emplace("_meta", json::RawValue{std::move(*encoded_meta)}).second)
        return std::unexpected(McpError::invalid_params("duplicate request metadata"));
    auto result = json::serialize(*values);
    if (!result) return std::unexpected(McpError::invalid_params(result.error()));
    return std::move(*result);
}
std::expected<ParsedRequest, McpError> parse_request(std::string_view body) {
    auto document = json::parse(body);
    if (!document) return std::unexpected(McpError::parse_error(document.error()));
    auto fields = json::decode<EnvelopeFields>(*document);
    if (!fields || fields->version != JSONRPC_VERSION || !fields->id || !fields->method)
        return std::unexpected(McpError::invalid_request(fields ? "missing or invalid envelope" : fields.error()));
    if (!fields->params) return std::unexpected(McpError::invalid_params("missing params"));
    auto params = json::deserialize<json::Json>(*fields->params);
    if (!params) return std::unexpected(McpError::invalid_params(params.error()));
    auto meta = json::decode_member<RequestMeta>(*params, "_meta");
    if (!meta) return std::unexpected(McpError::invalid_params(meta.error()));
    ParsedRequest parsed;
    parsed.request = {std::move(*fields->id), std::move(*fields->method), std::move(*params), std::move(*meta)};
    return parsed;
}
std::expected<ParsedResult, McpError> parse_result(std::string_view body) {
    auto type = json::deserialize_member<std::string>(body, "resultType");
    if (!type) return std::unexpected(McpError::invalid_response(type.error()));
    auto result = json::deserialize<json::Json>(body);
    if (!result) return std::unexpected(McpError::parse_error(result.error()));
    ParsedResult parsed;
    parsed.result.typeName = std::move(*type);
    parsed.result.type = parsed.result.typeName == "complete" ? ResultType::Complete :
        parsed.result.typeName == "input_required" ? ResultType::InputRequired : ResultType::Extension;
    parsed.result.result = std::move(*result);
    return parsed;
}
ToolCallResult ToolCallResult::text(std::string value) {
    ToolCallResult result;
    auto content = json::serialize(TextContent{"text", std::move(value)});
    if (content) result.content.push_back(std::move(*content));
    return result;
}
std::string ToolCallResult::encode() const {
    ToolResultFields fields;
    fields.content = content;
    fields.structured_content = structuredContent;
    fields.is_error = isError;
    if (serverInfo) fields.meta = ServerMeta{serverInfo};
    auto value = json::serialize(fields);
    if (!value) return {};
    return std::move(*value);
}
ReadResourceResult ReadResourceResult::text(std::string uri, std::string value, std::optional<std::string> mime_type) {
    ReadResourceResult result;
    auto content = json::serialize(TextResource{std::move(uri), std::move(value), std::move(mime_type)});
    if (content) result.contents.push_back(std::move(*content));
    return result;
}
std::string ReadResourceResult::encode() const {
    ResourceResultFields fields;
    fields.contents = contents;
    fields.ttl_ms = ttlMs;
    fields.cache_scope = cacheScope == CacheScope::Public ? "public" : "private";
    if (serverInfo) fields.meta = ServerMeta{serverInfo};
    auto value = json::serialize(fields);
    if (!value) return {};
    return std::move(*value);
}
std::string GetPromptResult::encode() const {
    PromptResultFields fields;
    fields.messages = messages;
    fields.description = description;
    if (serverInfo) fields.meta = ServerMeta{serverInfo};
    auto value = json::serialize(fields);
    if (!value) return {};
    return std::move(*value);
}
std::expected<ParsedResponse, McpError> parse_response(std::string_view body) {
    auto document = json::parse(body);
    if (!document) return std::unexpected(McpError::parse_error(document.error()));
    auto fields = json::decode<EnvelopeFields>(*document);
    if (!fields || fields->version != JSONRPC_VERSION)
        return std::unexpected(McpError::invalid_response(fields ? "missing or invalid jsonrpc" : fields.error()));
    if (fields->result.has_value() == fields->error.has_value())
        return std::unexpected(McpError::invalid_response("response must contain exactly one of result or error"));
    ParsedResponse parsed;
    if (fields->id) parsed.response.id = std::move(*fields->id);
    parsed.response.hasResult = fields->result.has_value();
    parsed.response.hasError = fields->error.has_value();
    if (fields->result) {
        if (!fields->id) return std::unexpected(McpError::invalid_response("result response missing id"));
        auto result = parse_result(*fields->result);
        if (!result) return std::unexpected(result.error());
        parsed.response.result = std::move(result->result.result);
    } else {
        auto error = json::deserialize<galay::mcp::JsonRpcError>(*fields->error);
        if (!error) return std::unexpected(McpError::invalid_response(error.error()));
        auto value = json::deserialize<json::Json>(*fields->error);
        if (!value) return std::unexpected(McpError::invalid_response(value.error()));
        parsed.response.error = std::move(*value);
    }
    return parsed;
}
std::string SubscriptionFilter::encode() const {
    auto value = json::serialize(*this);
    if (!value) return {};
    return std::move(*value);
}
std::expected<SubscriptionFilter, McpError> SubscriptionFilter::decode(const json::Json& element) {
    auto value = json::decode<SubscriptionFilter>(element);
    if (!value) return std::unexpected(McpError::invalid_params(value.error()));
    return std::move(*value);
}
std::string make_result_response(const RequestId& id, std::string_view result) {
    auto value = json::serialize(EnvelopeFields{JSONRPC_VERSION, id, {}, {}, std::string(result), {}});
    if (!value) return {};
    return std::move(*value);
}
std::string make_error_response(const std::optional<RequestId>& id, int code,
    std::string_view message, std::optional<std::string_view> details) {
    galay::mcp::JsonRpcError error;
    error.code = code;
    error.message = message;
    if (details) {
        auto data = json::serialize(*details);
        if (!data) return {};
        error.data = std::move(*data);
    }
    auto value = json::serialize(EnvelopeFields{JSONRPC_VERSION, id, {}, {}, {}, error.encode()});
    if (!value) return {};
    return std::move(*value);
}
std::string make_unsupported_protocol_version_response(const RequestId& id,
    std::string_view requested, const std::vector<std::string>& supported) {
    auto requested_value = json::serialize(requested);
    auto supported_value = json::serialize(supported);
    if (!requested_value || !supported_value) return {};
    auto data = json::serialize(json::Object{{"requested", {std::move(*requested_value)}},
        {"supported", {std::move(*supported_value)}}});
    if (!data) return {};
    galay::mcp::JsonRpcError error;
    error.code = ErrorCodes::UNSUPPORTED_PROTOCOL_VERSION;
    error.message = "Unsupported protocol version";
    error.data = std::move(*data);
    auto value = json::serialize(EnvelopeFields{JSONRPC_VERSION, id, {}, {}, {}, error.encode()});
    if (!value) return {};
    return std::move(*value);
}
std::string make_subscription_acknowledged_notification(const RequestId& id, const SubscriptionFilter& accepted) {
    auto params = json::serialize(SubscriptionParams{{id}, accepted, {}});
    if (!params) return {};
    auto value = json::serialize(EnvelopeFields{JSONRPC_VERSION, {}, NotificationMethods::SUBSCRIPTIONS_ACKNOWLEDGED,
        std::move(*params), {}, {}});
    if (!value) return {};
    return std::move(*value);
}
std::string make_subscription_notification(std::string_view method, const RequestId& id, std::optional<std::string_view> uri) {
    SubscriptionParams fields{{id}, {}, {}};
    if (uri) fields.uri = *uri;
    auto params = json::serialize(fields);
    if (!params) return {};
    auto value = json::serialize(EnvelopeFields{JSONRPC_VERSION, {}, std::string(method), std::move(*params), {}, {}});
    if (!value) return {};
    return std::move(*value);
}
std::string make_subscription_complete_response(const RequestId& id) {
    auto meta = json::serialize(SubscriptionMeta{id});
    if (!meta) return {};
    auto result = json::serialize(json::Object{{"resultType", {"\"complete\""}}, {"_meta", {std::move(*meta)}}});
    if (!result) return {};
    return make_result_response(id, *result);
}
std::string encode_sse_event(std::string_view message) {
    std::string event;
    event.reserve(message.size() + 8);
    std::size_t offset = 0;
    while (offset <= message.size()) {
        const std::size_t newline = message.find('\n', offset);
        event += "data: ";
        if (newline == std::string_view::npos) {
            event.append(message.substr(offset));
            event += "\n\n";
            break;
        }
        event.append(message.substr(offset, newline - offset));
        event.push_back('\n');
        offset = newline + 1;
    }
    return event;
}
std::expected<std::optional<std::string>, McpError> parse_sse_event(std::string_view event) {
    std::string data;
    bool has_data = false;
    std::size_t offset = 0;
    while (offset < event.size()) {
        std::size_t newline = event.find('\n', offset);
        if (newline == std::string_view::npos) newline = event.size();
        std::string_view line = event.substr(offset, newline - offset);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        offset = newline == event.size() ? event.size() : newline + 1;
        if (line.empty() || line.front() == ':') continue;
        const std::size_t colon = line.find(':');
        const std::string_view field = line.substr(0, colon);
        std::string_view value = colon == std::string_view::npos ? std::string_view{} : line.substr(colon + 1);
        if (!value.empty() && value.front() == ' ') value.remove_prefix(1);
        if (field != "data") continue;
        if (has_data) data.push_back('\n');
        data.append(value);
        has_data = true;
    }
    if (!has_data) return std::optional<std::string>{};
    auto message = json::deserialize<json::Json>(data);
    if (!message) return std::unexpected(McpError::invalid_response(message.error()));
    return std::optional<std::string>{std::move(data)};
}
} // namespace galay::mcp::v2
