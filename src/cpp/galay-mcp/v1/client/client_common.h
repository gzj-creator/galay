#ifndef GALAY_MCP_CLIENT_CLIENT_COMMON_H
#define GALAY_MCP_CLIENT_CLIENT_COMMON_H

#include "../../common/mcp_base.h"

#include <expected>
#include <string_view>
#include <utility>
#include <vector>

namespace galay::mcp::detail {

const std::string& empty_object_string();

template <typename T, typename ParseFn>
std::expected<std::vector<T>, McpError> parse_list_field(std::string_view body,
                                                       const char* fieldName,
                                                       ParseFn&& parse_fn) {
    auto docExp = JsonDocument::parse(body);
    if (!docExp) {
        return std::unexpected(McpError::parse_error(docExp.error().details()));
    }

    json::Json obj = docExp.value().root();
    if (!obj.is_object()) {
        return std::unexpected(McpError::parse_error("Expected JSON object"));
    }

    std::vector<T> values;
    json::Json arr = obj.at(fieldName);
    if (!arr.is_array()) {
        return values;
    }

    for (size_t i = 0; i < arr.size(); ++i) {
        const json::Json item = arr.at(i);
        auto parsed = parse_fn(item);
        if (!parsed) {
            return std::unexpected(McpError::parse_error(parsed.error().message()));
        }
        values.emplace_back(std::move(parsed.value()));
    }

    return values;
}

std::expected<InitializeResult, McpError> parse_initialize_result(std::string_view body);
std::expected<std::string, McpError> parse_tool_call_result(std::string_view body);
std::expected<std::string, McpError> parse_first_text_content(std::string_view body,
                                                           const char* fieldName);

} // namespace galay::mcp::detail

#endif
