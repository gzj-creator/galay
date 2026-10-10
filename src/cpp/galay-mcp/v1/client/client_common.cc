#include "client_common.h"

namespace galay::mcp::detail {

const std::string& empty_object_string() {
    static const std::string kEmptyObject = "{}";
    return kEmptyObject;
}

std::expected<InitializeResult, McpError> parse_initialize_result(std::string_view body) {
    auto initExp = json::deserialize<InitializeResult>(body);
    if (!initExp) {
        return std::unexpected(McpError::initialization_failed(initExp.error()));
    }

    return initExp.value();
}

std::expected<std::string, McpError> parse_tool_call_result(std::string_view body) {
    auto callExp = json::deserialize<ToolCallResult>(body);
    if (!callExp) {
        return std::unexpected(McpError::parse_error(callExp.error()));
    }

    const auto& callResult = callExp.value();
    if (callResult.isError) {
        return std::unexpected(McpError::tool_execution_failed("Tool returned error"));
    }
    if (callResult.content.empty()) {
        return empty_object_string();
    }
    if (callResult.content[0].type == ContentType::Text) {
        return callResult.content[0].text;
    }
    return empty_object_string();
}

std::expected<std::string, McpError> parse_first_text_content(std::string_view body,
                                                           const char* fieldName) {
    auto values = parse_list_field<Content>(body, fieldName);
    if (!values) return std::unexpected(values.error());
    for (const auto& content : *values)
        if (content.type == ContentType::Text) return content.text;

    return std::string();
}

} // namespace galay::mcp::detail
