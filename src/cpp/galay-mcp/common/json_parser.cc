#include "json_parser.h"

namespace galay::mcp {

namespace {

bool hasJsonRpcVersion(const JsonObject& obj) {
    std::string version;
    return JsonHelper::getString(obj, "jsonrpc", version) && version == JSONRPC_VERSION;
}

} // namespace

std::expected<ParsedJsonRpcRequest, McpError> parseJsonRpcRequest(std::string_view body) {
    auto docExp = JsonDocument::parse(body);
    if (!docExp) {
        return std::unexpected(docExp.error());
    }

    ParsedJsonRpcRequest parsed;
    parsed.document = std::move(docExp.value());

    JsonObject obj;
    if (!JsonHelper::getObject(parsed.document.root(), obj)) {
        return std::unexpected(McpError::invalidRequest("Expected JSON object"));
    }
    if (!hasJsonRpcVersion(obj)) {
        return std::unexpected(McpError::invalidRequest("Missing or invalid jsonrpc"));
    }

    auto methodVal = obj["method"];
    if (methodVal.error()) {
        return std::unexpected(McpError::invalidRequest("Missing method"));
    }
    auto methodStr = methodVal.value_unsafe().get_string();
    if (methodStr.error()) {
        return std::unexpected(McpError::invalidRequest("Invalid method type"));
    }
    parsed.request.method = std::string(methodStr.value_unsafe());

    auto idVal = obj["id"];
    if (!idVal.error()) {
        if (idVal.is_null()) {
            return std::unexpected(McpError::invalidRequest("Invalid id type"));
        }
        if (idVal.is_int64()) {
            parsed.request.id = idVal.get_int64().value_unsafe();
        } else {
            return std::unexpected(McpError::invalidRequest("Invalid id type"));
        }
    }

    auto paramsVal = obj["params"];
    if (!paramsVal.error() && !paramsVal.is_null()) {
        parsed.request.params = paramsVal.value_unsafe();
        parsed.request.hasParams = true;
    }

    return parsed;
}

std::expected<ParsedJsonRpcResponse, McpError> parseJsonRpcResponse(std::string_view body) {
    auto docExp = JsonDocument::parse(body);
    if (!docExp) {
        return std::unexpected(docExp.error());
    }

    ParsedJsonRpcResponse parsed;
    parsed.document = std::move(docExp.value());

    JsonObject obj;
    if (!JsonHelper::getObject(parsed.document.root(), obj)) {
        return std::unexpected(McpError::invalidResponse("Expected JSON object"));
    }
    if (!hasJsonRpcVersion(obj)) {
        return std::unexpected(McpError::invalidResponse("Missing or invalid jsonrpc"));
    }

    auto idVal = obj["id"];
    if (idVal.error() || !idVal.is_int64()) {
        return std::unexpected(McpError::invalidResponse("Missing or invalid id"));
    }
    parsed.response.id = idVal.get_int64().value_unsafe();

    auto resultVal = obj["result"];
    if (!resultVal.error()) {
        parsed.response.result = resultVal.value_unsafe();
        parsed.response.hasResult = true;
    }

    auto errorVal = obj["error"];
    if (!errorVal.error()) {
        auto errorExp = JsonRpcError::fromJson(errorVal.value_unsafe());
        if (!errorExp) {
            return std::unexpected(McpError::invalidResponse("Malformed error object"));
        }
        parsed.response.error = errorVal.value_unsafe();
        parsed.response.hasError = true;
    }

    if (parsed.response.hasResult == parsed.response.hasError) {
        return std::unexpected(McpError::invalidResponse("Response must contain exactly one of result or error"));
    }

    return parsed;
}

} // namespace galay::mcp
