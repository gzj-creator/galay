#include "galay-mcp/common/json_parser.h"

namespace galay {
namespace mcp {

std::expected<ParsedJsonRpcRequest, McpError> parseJsonRpcRequest(std::string_view body) {
    auto docExp = JsonDocument::Parse(body);
    if (!docExp) {
        return std::unexpected(docExp.error());
    }

    ParsedJsonRpcRequest parsed;
    parsed.document = std::move(docExp.value());

    JsonObject obj;
    if (!JsonHelper::GetObject(parsed.document.Root(), obj)) {
        return std::unexpected(McpError::invalidRequest("Expected JSON object"));
    }

    auto methodVal = obj["method"];
    if (methodVal.error()) {
        return std::unexpected(McpError::invalidRequest("Missing method"));
    }
    auto methodStr = methodVal.value().get_string();
    if (methodStr.error()) {
        return std::unexpected(McpError::invalidRequest("Invalid method type"));
    }
    parsed.request.method = std::string(methodStr.value());

    auto idVal = obj["id"];
    if (!idVal.error() && !idVal.is_null()) {
        if (idVal.is_int64()) {
            parsed.request.id = idVal.get_int64().value();
        } else {
            return std::unexpected(McpError::invalidRequest("Invalid id type"));
        }
    }

    auto paramsVal = obj["params"];
    if (!paramsVal.error() && !paramsVal.is_null()) {
        parsed.request.params = paramsVal.value();
        parsed.request.hasParams = true;
    }

    return parsed;
}

std::expected<ParsedJsonRpcResponse, McpError> parseJsonRpcResponse(std::string_view body) {
    auto docExp = JsonDocument::Parse(body);
    if (!docExp) {
        return std::unexpected(docExp.error());
    }

    ParsedJsonRpcResponse parsed;
    parsed.document = std::move(docExp.value());

    JsonObject obj;
    if (!JsonHelper::GetObject(parsed.document.Root(), obj)) {
        return std::unexpected(McpError::invalidResponse("Expected JSON object"));
    }

    auto idVal = obj["id"];
    if (idVal.error() || !idVal.is_int64()) {
        return std::unexpected(McpError::invalidResponse("Missing or invalid id"));
    }
    parsed.response.id = idVal.get_int64().value();

    auto resultVal = obj["result"];
    if (!resultVal.error() && !resultVal.is_null()) {
        parsed.response.result = resultVal.value();
        parsed.response.hasResult = true;
    }

    auto errorVal = obj["error"];
    if (!errorVal.error() && !errorVal.is_null()) {
        parsed.response.error = errorVal.value();
        parsed.response.hasError = true;
    }

    return parsed;
}

} // namespace mcp
} // namespace galay
