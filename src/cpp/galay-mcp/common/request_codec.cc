#include "request_codec.h"

namespace galay::mcp {
namespace {
struct RequestFields {
    std::string version;
    std::optional<std::variant<int64_t, std::nullptr_t>> id;
    std::string method;
    json::Json params;
};
constexpr auto reflect_fields(std::type_identity<RequestFields>) {
    return std::make_tuple(json::make_field("jsonrpc", &RequestFields::version),
        json::make_field("id", &RequestFields::id, json::FieldPolicy{.reject_null = true}), json::make_field("method", &RequestFields::method),
        json::make_field("params", &RequestFields::params, json::FieldPolicy{.optional = true}));
}
struct ResponseFields {
    std::string version;
    int64_t id;
    std::optional<std::string> result;
    std::optional<JsonRpcError> error;
};
constexpr auto reflect_fields(std::type_identity<ResponseFields>) {
    return std::make_tuple(json::make_field("jsonrpc", &ResponseFields::version),
        json::make_field("id", &ResponseFields::id),
        json::make_field("result", &ResponseFields::result, json::FieldPolicy{.raw = true}),
        json::make_field("error", &ResponseFields::error, json::FieldPolicy{.reject_null = true}));
}
}

std::expected<ParsedJsonRpcRequest, McpError> parse_json_rpc_request(std::string_view body) {
    auto document = json::parse(body);
    if (!document) return std::unexpected(McpError::parse_error(document.error()));
    auto fields = json::decode<RequestFields>(*document);
    if (!fields || fields->version != JSONRPC_VERSION)
        return std::unexpected(McpError::invalid_request(fields ? "Invalid jsonrpc" : fields.error()));
    ParsedJsonRpcRequest parsed;
    if (fields->id) {
        const auto* id = std::get_if<int64_t>(&*fields->id);
        if (!id) return std::unexpected(McpError::invalid_request("Invalid id type"));
        parsed.request.id = *id;
    }
    parsed.request.method = std::move(fields->method);
    parsed.request.params = std::move(fields->params);
    parsed.request.hasParams = parsed.request.params.valid() && !parsed.request.params.is_null();
    return parsed;
}

std::expected<ParsedJsonRpcResponse, McpError> parse_json_rpc_response(std::string_view body) {
    auto document = json::parse(body);
    if (!document) return std::unexpected(McpError::parse_error(document.error()));
    auto fields = json::decode<ResponseFields>(*document);
    if (!fields || fields->version != JSONRPC_VERSION)
        return std::unexpected(McpError::invalid_response(fields ? "Invalid jsonrpc" : fields.error()));
    if (fields->result.has_value() == fields->error.has_value())
        return std::unexpected(McpError::invalid_response("Response must contain exactly one of result or error"));
    ParsedJsonRpcResponse parsed;
    parsed.response.id = fields->id;
    parsed.response.hasResult = fields->result.has_value();
    parsed.response.hasError = fields->error.has_value();
    if (fields->result) {
        auto result = json::parse(*fields->result);
        if (!result) return std::unexpected(McpError::invalid_response(result.error()));
        parsed.response.result = std::move(*result);
    }
    if (fields->error) {
        auto error = json::serialize(*fields->error);
        if (!error) return std::unexpected(McpError::invalid_response(error.error()));
        auto value = json::parse(*error);
        if (!value) return std::unexpected(McpError::invalid_response(value.error()));
        parsed.response.error = std::move(*value);
    }
    return parsed;
}
} // namespace galay::mcp
