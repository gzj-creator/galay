#include "client_common.h"

namespace galay::mcp::detail {

const std::string& empty_object_string() {
    static const std::string kEmptyObject = "{}";
    return kEmptyObject;
}

std::expected<InitializeResult, McpError> parse_initialize_result(std::string_view body) {
    auto docExp = JsonDocument::parse(body);
    if (!docExp) {
        return std::unexpected(McpError::parse_error(docExp.error().details()));
    }

    auto initExp = InitializeResult::from_json(docExp.value().root());
    if (!initExp) {
        return std::unexpected(McpError::initialization_failed(initExp.error().message()));
    }

    return initExp.value();
}

std::expected<std::string, McpError> parse_tool_call_result(std::string_view body) {
    auto docExp = JsonDocument::parse(body);
    if (!docExp) {
        return std::unexpected(McpError::parse_error(docExp.error().details()));
    }

    auto callExp = ToolCallResult::from_json(docExp.value().root());
    if (!callExp) {
        return std::unexpected(McpError::parse_error(callExp.error().message()));
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
    auto docExp = JsonDocument::parse(body);
    if (!docExp) {
        return std::unexpected(McpError::parse_error(docExp.error().details()));
    }

    json::Json obj = docExp.value().root();
    if (!obj.is_object()) {
        return std::unexpected(McpError::parse_error("Expected JSON object"));
    }

    json::Json arr = obj.at(fieldName);
    if (!arr.is_array()) {
        return std::string();
    }

    for (size_t i = 0; i < arr.size(); ++i) {
        const json::Json item = arr.at(i);
        auto contentExp = Content::from_json(item);
        if (!contentExp) {
            return std::unexpected(McpError::parse_error(contentExp.error().message()));
        }
        if (contentExp.value().type == ContentType::Text) {
            return contentExp.value().text;
        }
    }

    return std::string();
}

} // namespace galay::mcp::detail
