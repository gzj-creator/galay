#ifndef GALAY_MCP_CLIENT_CLIENT_COMMON_H
#define GALAY_MCP_CLIENT_CLIENT_COMMON_H

#include "../../common/mcp_base.h"

#include <expected>
#include <string_view>
#include <utility>
#include <vector>

namespace galay::mcp::detail {

const std::string& emptyObjectString();

template <typename T, typename ParseFn>
std::expected<std::vector<T>, McpError> parseListField(std::string_view body,
                                                       const char* fieldName,
                                                       ParseFn&& parseFn) {
    auto docExp = JsonDocument::parse(body);
    if (!docExp) {
        return std::unexpected(McpError::parseError(docExp.error().details()));
    }

    json::Json obj = docExp.value().root();
    if (!obj.is_object()) {
        return std::unexpected(McpError::parseError("Expected JSON object"));
    }

    std::vector<T> values;
    json::Json arr = obj.at(fieldName);
    if (!arr.is_array()) {
        return values;
    }

    for (size_t i = 0; i < arr.size(); ++i) {
        const json::Json item = arr.at(i);
        auto parsed = parseFn(item);
        if (!parsed) {
            return std::unexpected(McpError::parseError(parsed.error().message()));
        }
        values.emplace_back(std::move(parsed.value()));
    }

    return values;
}

std::expected<InitializeResult, McpError> parseInitializeResult(std::string_view body);
std::expected<std::string, McpError> parseToolCallResult(std::string_view body);
std::expected<std::string, McpError> parseFirstTextContent(std::string_view body,
                                                           const char* fieldName);

} // namespace galay::mcp::detail

#endif
