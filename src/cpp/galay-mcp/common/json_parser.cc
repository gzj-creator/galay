#include "json_parser.h"

namespace galay::mcp {

namespace {

bool has_json_rpc_version(const json::Json& obj) {
    auto version = obj.at("jsonrpc").as_string();
    return version.has_value() && *version == JSONRPC_VERSION;
}

} // namespace

std::expected<ParsedJsonRpcRequest, McpError> parse_json_rpc_request(std::string_view body) {
    auto docExp = JsonDocument::parse(body);
    if (!docExp) {
        return std::unexpected(docExp.error());
    }

    ParsedJsonRpcRequest parsed;
    parsed.document = std::move(docExp.value());

    json::Json obj = parsed.document.root();
    if (!obj.is_object()) {
        return std::unexpected(McpError::invalid_request("Expected JSON object"));
    }
    if (!has_json_rpc_version(obj)) {
        return std::unexpected(McpError::invalid_request("Missing or invalid jsonrpc"));
    }

    auto methodVal = obj["method"];
    if (!methodVal.valid()) {
        return std::unexpected(McpError::invalid_request("Missing method"));
    }
    auto methodStr = methodVal.as_string();
    if (!methodStr) {
        return std::unexpected(McpError::invalid_request("Invalid method type"));
    }
    parsed.request.method = std::string(methodStr.value());

    auto idVal = obj["id"];
    if (idVal.valid()) {
        if (idVal.is_null()) {
            return std::unexpected(McpError::invalid_request("Invalid id type"));
        }
        auto idNum = idVal.as_int64();
        if (idNum) {
            parsed.request.id = idNum.value();
        } else {
            return std::unexpected(McpError::invalid_request("Invalid id type"));
        }
    }

    auto paramsVal = obj["params"];
    if (paramsVal.valid() && !paramsVal.is_null()) {
        parsed.request.params = paramsVal;
        parsed.request.hasParams = true;
    }

    return parsed;
}

std::expected<ParsedJsonRpcResponse, McpError> parse_json_rpc_response(std::string_view body) {
    auto docExp = JsonDocument::parse(body);
    if (!docExp) {
        return std::unexpected(docExp.error());
    }

    ParsedJsonRpcResponse parsed;
    parsed.document = std::move(docExp.value());

    json::Json obj = parsed.document.root();
    if (!obj.is_object()) {
        return std::unexpected(McpError::invalid_response("Expected JSON object"));
    }
    if (!has_json_rpc_version(obj)) {
        return std::unexpected(McpError::invalid_response("Missing or invalid jsonrpc"));
    }

    auto idVal = obj["id"];
    auto idNum = idVal.as_int64();
    if (!idNum) {
        return std::unexpected(McpError::invalid_response("Missing or invalid id"));
    }
    parsed.response.id = idNum.value();

    auto resultVal = obj["result"];
    if (resultVal.valid()) {
        parsed.response.result = resultVal;
        parsed.response.hasResult = true;
    }

    auto errorVal = obj["error"];
    if (errorVal.valid()) {
        auto errorExp = JsonRpcError::from_json(errorVal);
        if (!errorExp) {
            return std::unexpected(McpError::invalid_response("Malformed error object"));
        }
        parsed.response.error = errorVal;
        parsed.response.hasError = true;
    }

    if (parsed.response.hasResult == parsed.response.hasError) {
        return std::unexpected(McpError::invalid_response("Response must contain exactly one of result or error"));
    }

    return parsed;
}

} // namespace galay::mcp
