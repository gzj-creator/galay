#include "mcp_base.h"

namespace galay::mcp {

std::string Content::encode() const {
    auto value = json::serialize(*this);
    if (!value) return {};
    return std::move(*value);
}

std::string Tool::encode() const {
    auto value = json::serialize(*this);
    if (!value) return {};
    return std::move(*value);
}

std::string Resource::encode() const {
    auto value = json::serialize(*this);
    if (!value) return {};
    return std::move(*value);
}

std::string PromptArgument::encode() const {
    auto value = json::serialize(*this);
    if (!value) return {};
    return std::move(*value);
}

std::string Prompt::encode() const {
    auto value = json::serialize(*this);
    if (!value) return {};
    return std::move(*value);
}

std::string ClientInfo::encode() const {
    auto value = json::serialize(*this);
    if (!value) return {};
    return std::move(*value);
}

std::string ServerInfo::encode() const {
    auto value = json::serialize(*this);
    if (!value) return {};
    return std::move(*value);
}

std::string ServerCapabilities::encode() const {
    auto value = json::serialize(*this);
    if (!value) return {};
    return std::move(*value);
}

std::string InitializeParams::encode() const {
    auto value = json::serialize(*this);
    if (!value) return {};
    return std::move(*value);
}

std::string InitializeResult::encode() const {
    auto value = json::serialize(*this);
    if (!value) return {};
    return std::move(*value);
}

std::string ToolCallParams::encode() const {
    auto value = json::serialize(*this);
    if (!value) return {};
    return std::move(*value);
}

std::string ToolCallResult::encode() const {
    auto value = json::serialize(*this);
    if (!value) return {};
    return std::move(*value);
}

std::string JsonRpcRequest::encode() const {
    auto value = json::serialize(*this);
    if (!value) return {};
    return std::move(*value);
}

std::string JsonRpcResponse::encode() const {
    auto value = json::serialize(*this);
    if (!value) return {};
    return std::move(*value);
}

std::string JsonRpcNotification::encode() const {
    auto value = json::serialize(*this);
    if (!value) return {};
    return std::move(*value);
}

std::string JsonRpcError::encode() const {
    auto value = json::serialize(*this);
    if (!value) return {};
    return std::move(*value);
}

std::expected<Content, McpError> Content::decode(const json::Json& element) {
    auto value = json::decode<Content>(element);
    if (!value) return std::unexpected(McpError::invalid_message(value.error()));
    return std::move(*value);
}

std::expected<Tool, McpError> Tool::decode(const json::Json& element) {
    auto value = json::decode<Tool>(element);
    if (!value) return std::unexpected(McpError::invalid_message(value.error()));
    return std::move(*value);
}

std::expected<Resource, McpError> Resource::decode(const json::Json& element) {
    auto value = json::decode<Resource>(element);
    if (!value) return std::unexpected(McpError::invalid_message(value.error()));
    return std::move(*value);
}

std::expected<PromptArgument, McpError> PromptArgument::decode(const json::Json& element) {
    auto value = json::decode<PromptArgument>(element);
    if (!value) return std::unexpected(McpError::invalid_message(value.error()));
    return std::move(*value);
}

std::expected<Prompt, McpError> Prompt::decode(const json::Json& element) {
    auto value = json::decode<Prompt>(element);
    if (!value) return std::unexpected(McpError::invalid_message(value.error()));
    return std::move(*value);
}

std::expected<ClientInfo, McpError> ClientInfo::decode(const json::Json& element) {
    auto value = json::decode<ClientInfo>(element);
    if (!value) return std::unexpected(McpError::invalid_message(value.error()));
    return std::move(*value);
}

std::expected<ServerInfo, McpError> ServerInfo::decode(const json::Json& element) {
    auto value = json::decode<ServerInfo>(element);
    if (!value) return std::unexpected(McpError::invalid_message(value.error()));
    return std::move(*value);
}

std::expected<ServerCapabilities, McpError> ServerCapabilities::decode(const json::Json& element) {
    auto value = json::decode<ServerCapabilities>(element);
    if (!value) return std::unexpected(McpError::invalid_message(value.error()));
    return std::move(*value);
}

std::expected<InitializeParams, McpError> InitializeParams::decode(const json::Json& element) {
    auto value = json::decode<InitializeParams>(element);
    if (!value) return std::unexpected(McpError::invalid_message(value.error()));
    return std::move(*value);
}

std::expected<InitializeResult, McpError> InitializeResult::decode(const json::Json& element) {
    auto value = json::decode<InitializeResult>(element);
    if (!value) return std::unexpected(McpError::invalid_message(value.error()));
    return std::move(*value);
}

std::expected<ToolCallParams, McpError> ToolCallParams::decode(const json::Json& element) {
    auto value = json::decode<ToolCallParams>(element);
    if (!value) return std::unexpected(McpError::invalid_message(value.error()));
    return std::move(*value);
}

std::expected<ToolCallResult, McpError> ToolCallResult::decode(const json::Json& element) {
    auto value = json::decode<ToolCallResult>(element);
    if (!value) return std::unexpected(McpError::invalid_message(value.error()));
    return std::move(*value);
}

std::expected<JsonRpcResponse, McpError> JsonRpcResponse::decode(const json::Json& element) {
    auto value = json::decode<JsonRpcResponse>(element);
    if (!value) return std::unexpected(McpError::invalid_message(value.error()));
    return std::move(*value);
}

std::expected<JsonRpcError, McpError> JsonRpcError::decode(const json::Json& element) {
    auto value = json::decode<JsonRpcError>(element);
    if (!value) return std::unexpected(McpError::invalid_message(value.error()));
    return std::move(*value);
}

} // namespace galay::mcp
