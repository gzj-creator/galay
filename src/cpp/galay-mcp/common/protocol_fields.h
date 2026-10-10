#ifndef GALAY_MCP_PROTOCOL_FIELDS_H
#define GALAY_MCP_PROTOCOL_FIELDS_H

#include "mcp_base.h"
#include <tuple>

namespace galay::mcp {

struct NamedArguments {
    std::string name;
    json::Json arguments = json::empty_object();
};
constexpr auto reflect_fields(std::type_identity<NamedArguments>) {
    return std::make_tuple(json::make_field("name", &NamedArguments::name),
        json::make_field("arguments", &NamedArguments::arguments, json::FieldPolicy{.optional = true}));
}
struct ResourceParams {
    std::string uri;
};
constexpr auto reflect_fields(std::type_identity<ResourceParams>) {
    return std::make_tuple(json::make_field("uri", &ResourceParams::uri));
}
struct PromptParams {
    std::string name;
    std::optional<std::string> arguments;
};
constexpr auto reflect_fields(std::type_identity<PromptParams>) {
    return std::make_tuple(json::make_field("name", &PromptParams::name),
        json::make_field("arguments", &PromptParams::arguments, json::FieldPolicy{.raw = true, .omit_empty = true}));
}

constexpr auto reflect_fields(std::type_identity<Tool>) {
    return std::make_tuple(json::make_field("name", &Tool::name),
        json::make_field("description", &Tool::description),
        json::make_field("inputSchema", &Tool::inputSchema,
            json::FieldPolicy{.raw = true, .optional = true, .empty_object = true}));
}
constexpr auto reflect_fields(std::type_identity<Resource>) {
    return std::make_tuple(json::make_field("uri", &Resource::uri),
        json::make_field("name", &Resource::name), json::make_field("description", &Resource::description),
        json::make_field("mimeType", &Resource::mimeType));
}
constexpr auto reflect_fields(std::type_identity<PromptArgument>) {
    return std::make_tuple(json::make_field("name", &PromptArgument::name),
        json::make_field("description", &PromptArgument::description),
        json::make_field("required", &PromptArgument::required, json::FieldPolicy{.optional = true}));
}
constexpr auto reflect_fields(std::type_identity<Prompt>) {
    return std::make_tuple(json::make_field("name", &Prompt::name),
        json::make_field("description", &Prompt::description),
        json::make_field("arguments", &Prompt::arguments, json::FieldPolicy{.optional = true}));
}
constexpr auto reflect_fields(std::type_identity<ClientInfo>) {
    return std::make_tuple(json::make_field("name", &ClientInfo::name), json::make_field("version", &ClientInfo::version));
}
constexpr auto reflect_fields(std::type_identity<ServerInfo>) {
    return std::make_tuple(json::make_field("name", &ServerInfo::name), json::make_field("version", &ServerInfo::version),
        json::make_field("capabilities", &ServerInfo::capabilities,
            json::FieldPolicy{.raw = true, .optional = true, .empty_object = true}));
}
constexpr auto reflect_fields(std::type_identity<ServerCapabilities>) {
    return std::make_tuple(
        json::make_field("tools", &ServerCapabilities::tools, json::FieldPolicy{.optional = true, .presence_object = true}),
        json::make_field("resources", &ServerCapabilities::resources, json::FieldPolicy{.optional = true, .presence_object = true}),
        json::make_field("prompts", &ServerCapabilities::prompts, json::FieldPolicy{.optional = true, .presence_object = true}),
        json::make_field("logging", &ServerCapabilities::logging, json::FieldPolicy{.optional = true, .presence_object = true}));
}
constexpr auto reflect_fields(std::type_identity<InitializeParams>) {
    return std::make_tuple(json::make_field("protocolVersion", &InitializeParams::protocolVersion),
        json::make_field("clientInfo", &InitializeParams::clientInfo),
        json::make_field("capabilities", &InitializeParams::capabilities,
            json::FieldPolicy{.raw = true, .optional = true, .empty_object = true}));
}
constexpr auto reflect_fields(std::type_identity<InitializeResult>) {
    return std::make_tuple(json::make_field("protocolVersion", &InitializeResult::protocolVersion),
        json::make_field("serverInfo", &InitializeResult::serverInfo), json::make_field("capabilities", &InitializeResult::capabilities));
}
constexpr auto reflect_fields(std::type_identity<ToolCallParams>) {
    return std::make_tuple(json::make_field("name", &ToolCallParams::name),
        json::make_field("arguments", &ToolCallParams::arguments,
            json::FieldPolicy{.raw = true, .optional = true, .empty_object = true}));
}
constexpr auto reflect_fields(std::type_identity<ToolCallResult>) {
    return std::make_tuple(json::make_field("content", &ToolCallResult::content, json::FieldPolicy{.optional = true}),
        json::make_field("isError", &ToolCallResult::isError, json::FieldPolicy{.optional = true, .omit_false = true}));
}
constexpr auto reflect_fields(std::type_identity<JsonRpcRequest>) {
    return std::make_tuple(json::make_field("jsonrpc", &JsonRpcRequest::jsonrpc),
        json::make_field("id", &JsonRpcRequest::id, json::FieldPolicy{.omit_empty = true}),
        json::make_field("method", &JsonRpcRequest::method),
        json::make_field("params", &JsonRpcRequest::params, json::FieldPolicy{.raw = true, .omit_empty = true}));
}
constexpr auto reflect_fields(std::type_identity<JsonRpcResponse>) {
    return std::make_tuple(json::make_field("jsonrpc", &JsonRpcResponse::jsonrpc), json::make_field("id", &JsonRpcResponse::id),
        json::make_field("result", &JsonRpcResponse::result, json::FieldPolicy{.raw = true, .omit_empty = true}),
        json::make_field("error", &JsonRpcResponse::error, json::FieldPolicy{.raw = true, .omit_empty = true}));
}
constexpr auto reflect_fields(std::type_identity<JsonRpcNotification>) {
    return std::make_tuple(json::make_field("jsonrpc", &JsonRpcNotification::jsonrpc),
        json::make_field("method", &JsonRpcNotification::method),
        json::make_field("params", &JsonRpcNotification::params, json::FieldPolicy{.raw = true, .omit_empty = true}));
}
constexpr auto reflect_fields(std::type_identity<JsonRpcError>) {
    return std::make_tuple(json::make_field("code", &JsonRpcError::code), json::make_field("message", &JsonRpcError::message),
        json::make_field("data", &JsonRpcError::data, json::FieldPolicy{.raw = true, .omit_empty = true}));
}

struct ContentFields {
    std::string type;
    std::optional<std::string> text;
    std::optional<std::string> data;
    std::optional<std::string> mime_type;
    std::optional<std::string> uri;
};
constexpr auto reflect_fields(std::type_identity<ContentFields>) {
    return std::make_tuple(json::make_field("type", &ContentFields::type),
        json::make_field("text", &ContentFields::text, json::FieldPolicy{.omit_empty = true}),
        json::make_field("data", &ContentFields::data, json::FieldPolicy{.omit_empty = true}),
        json::make_field("mimeType", &ContentFields::mime_type, json::FieldPolicy{.omit_empty = true}),
        json::make_field("uri", &ContentFields::uri, json::FieldPolicy{.omit_empty = true}));
}
inline ContentFields to_wire(const Content& value) {
    switch (value.type) {
        case ContentType::Text: return {"text", value.text, {}, {}, {}};
        case ContentType::Image: return {"image", {}, value.data, value.mimeType, {}};
        case ContentType::Resource: return {"resource", {}, {}, {}, value.uri};
    }
    return {};
}
inline auto wire_type(std::type_identity<Content>) { return std::type_identity<ContentFields>{}; }
inline json::result<Content> from_wire(std::type_identity<Content>, ContentFields value) {
    Content content;
    if (value.type == "text" && value.text) {
        content.type = ContentType::Text;
        content.text = std::move(*value.text);
    } else if (value.type == "image" && value.data && value.mime_type) {
        content.type = ContentType::Image;
        content.data = std::move(*value.data);
        content.mimeType = std::move(*value.mime_type);
    } else if (value.type == "resource" && value.uri) {
        content.type = ContentType::Resource;
        content.uri = std::move(*value.uri);
    } else {
        return std::unexpected(std::string("Unknown or incomplete content type"));
    }
    return content;
}

} // namespace galay::mcp
#endif
