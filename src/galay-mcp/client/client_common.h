#ifndef GALAY_MCP_CLIENT_CLIENT_COMMON_H
#define GALAY_MCP_CLIENT_CLIENT_COMMON_H

#include "galay-mcp/common/mcp_base.h"

#include <expected>
#include <string_view>
#include <utility>
#include <vector>

namespace galay::mcp::detail {

const JsonString& EmptyObjectString();

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

std::expected<InitializeResult, McpError> parseInitializeResult(std::string_view body);
std::expected<JsonString, McpError> parseToolCallResult(std::string_view body);
std::expected<std::string, McpError> parseFirstTextContent(std::string_view body,
                                                           const char* fieldName);

} // namespace galay::mcp::detail

#endif
