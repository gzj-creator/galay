#ifndef GALAY_MCP_CLIENT_CLIENT_COMMON_H
#define GALAY_MCP_CLIENT_CLIENT_COMMON_H

#include "../../common/mcp_base.h"

#include <expected>
#include <string_view>
#include <utility>
#include <vector>

namespace galay::mcp::detail {

const std::string& empty_object_string();

template <typename T>
std::expected<std::vector<T>, McpError> parse_list_field(std::string_view body,
                                                       const char* field_name) {
    auto values = json::deserialize_member<std::optional<std::vector<T>>>(body, field_name);
    if (!values) return std::unexpected(McpError::parse_error(values.error()));
    return std::move(*values).value_or(std::vector<T>{});
}

std::expected<InitializeResult, McpError> parse_initialize_result(std::string_view body);
std::expected<std::string, McpError> parse_tool_call_result(std::string_view body);
std::expected<std::string, McpError> parse_first_text_content(std::string_view body,
                                                           const char* fieldName);

} // namespace galay::mcp::detail

#endif
