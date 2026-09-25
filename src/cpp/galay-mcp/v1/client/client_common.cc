#include "client_common.h"

namespace galay::mcp::detail {

const std::string& emptyObjectString() {
    static const std::string kEmptyObject = "{}";
    return kEmptyObject;
}

std::expected<InitializeResult, McpError> parseInitializeResult(std::string_view body) {
    auto docExp = JsonDocument::parse(body);
    if (!docExp) {
        return std::unexpected(McpError::parseError(docExp.error().details()));
    }

    auto initExp = InitializeResult::fromJson(docExp.value().root());
    if (!initExp) {
        return std::unexpected(McpError::initializationFailed(initExp.error().message()));
    }

    return initExp.value();
}

std::expected<std::string, McpError> parseToolCallResult(std::string_view body) {
    auto docExp = JsonDocument::parse(body);
    if (!docExp) {
        return std::unexpected(McpError::parseError(docExp.error().details()));
    }

    auto callExp = ToolCallResult::fromJson(docExp.value().root());
    if (!callExp) {
        return std::unexpected(McpError::parseError(callExp.error().message()));
    }

    const auto& callResult = callExp.value();
    if (callResult.isError) {
        return std::unexpected(McpError::toolExecutionFailed("Tool returned error"));
    }
    if (callResult.content.empty()) {
        return emptyObjectString();
    }
    if (callResult.content[0].type == ContentType::Text) {
        return callResult.content[0].text;
    }
    return emptyObjectString();
}

std::expected<std::string, McpError> parseFirstTextContent(std::string_view body,
                                                           const char* fieldName) {
    auto docExp = JsonDocument::parse(body);
    if (!docExp) {
        return std::unexpected(McpError::parseError(docExp.error().details()));
    }

    json::Json obj = docExp.value().root();
    if (!obj.is_object()) {
        return std::unexpected(McpError::parseError("Expected JSON object"));
    }

    json::Json arr = obj.at(fieldName);
    if (!arr.is_array()) {
        return std::string();
    }

    for (size_t i = 0; i < arr.size(); ++i) {
        const json::Json item = arr.at(i);
        auto contentExp = Content::fromJson(item);
        if (!contentExp) {
            return std::unexpected(McpError::parseError(contentExp.error().message()));
        }
        if (contentExp.value().type == ContentType::Text) {
            return contentExp.value().text;
        }
    }

    return std::string();
}

} // namespace galay::mcp::detail
