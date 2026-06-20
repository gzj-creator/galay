#include <galay/cpp/galay-mcp/client/client_common.h>

namespace galay::mcp::detail {

const JsonString& EmptyObjectString() {
    static const JsonString kEmptyObject = "{}";
    return kEmptyObject;
}

std::expected<InitializeResult, McpError> parseInitializeResult(std::string_view body) {
    auto docExp = JsonDocument::Parse(body);
    if (!docExp) {
        return std::unexpected(McpError::parseError(docExp.error().details()));
    }

    auto initExp = InitializeResult::fromJson(docExp.value().Root());
    if (!initExp) {
        return std::unexpected(McpError::initializationFailed(initExp.error().message()));
    }

    return initExp.value();
}

std::expected<JsonString, McpError> parseToolCallResult(std::string_view body) {
    auto docExp = JsonDocument::Parse(body);
    if (!docExp) {
        return std::unexpected(McpError::parseError(docExp.error().details()));
    }

    auto callExp = ToolCallResult::fromJson(docExp.value().Root());
    if (!callExp) {
        return std::unexpected(McpError::parseError(callExp.error().message()));
    }

    const auto& callResult = callExp.value();
    if (callResult.isError) {
        return std::unexpected(McpError::toolExecutionFailed("Tool returned error"));
    }
    if (callResult.content.empty()) {
        return EmptyObjectString();
    }
    if (callResult.content[0].type == ContentType::Text) {
        return callResult.content[0].text;
    }
    return EmptyObjectString();
}

std::expected<std::string, McpError> parseFirstTextContent(std::string_view body,
                                                           const char* fieldName) {
    auto docExp = JsonDocument::Parse(body);
    if (!docExp) {
        return std::unexpected(McpError::parseError(docExp.error().details()));
    }

    JsonObject obj;
    if (!JsonHelper::GetObject(docExp.value().Root(), obj)) {
        return std::unexpected(McpError::parseError("Expected JSON object"));
    }

    JsonArray arr;
    if (!JsonHelper::GetArray(obj, fieldName, arr)) {
        return std::string();
    }

    for (auto item : arr) {
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
