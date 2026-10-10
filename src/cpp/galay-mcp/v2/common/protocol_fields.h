#ifndef GALAY_MCP_V2_PROTOCOL_FIELDS_H
#define GALAY_MCP_V2_PROTOCOL_FIELDS_H
#include "protocol.h"
#include <tuple>

namespace galay::mcp::v2 {
struct ArgumentsFields {
    json::Json arguments = json::empty_object();
};
constexpr auto reflect_fields(std::type_identity<ArgumentsFields>) {
    return std::make_tuple(json::make_field("arguments", &ArgumentsFields::arguments,
        json::FieldPolicy{.optional = true, .object_only = true}));
}

constexpr auto reflect_fields(std::type_identity<Implementation>) {
    return std::make_tuple(json::make_field("name", &Implementation::name),
        json::make_field("version", &Implementation::version),
        json::make_field("title", &Implementation::title, json::FieldPolicy{.omit_empty = true}),
        json::make_field("description", &Implementation::description, json::FieldPolicy{.omit_empty = true}),
        json::make_field("websiteUrl", &Implementation::websiteUrl, json::FieldPolicy{.omit_empty = true}),
        json::make_field("icons", &Implementation::icons, json::FieldPolicy{.raw = true, .optional = true, .omit_empty = true}));
}
constexpr auto reflect_fields(std::type_identity<RequestMeta>) {
    return std::make_tuple(
        json::make_field("io.modelcontextprotocol/protocolVersion", &RequestMeta::protocolVersion),
        json::make_field("io.modelcontextprotocol/clientCapabilities", &RequestMeta::clientCapabilities,
            json::FieldPolicy{.raw = true, .object_only = true, .empty_object = true}),
        json::make_field("io.modelcontextprotocol/clientInfo", &RequestMeta::clientInfo, json::FieldPolicy{.omit_empty = true, .reject_null = true}),
        json::make_field("io.modelcontextprotocol/logLevel", &RequestMeta::logLevel, json::FieldPolicy{.omit_empty = true}),
        json::make_field("progressToken", &RequestMeta::progressToken, json::FieldPolicy{.omit_empty = true, .reject_null = true}));
}
constexpr auto reflect_fields(std::type_identity<Tool>) {
    return std::make_tuple(json::make_field("name", &Tool::name),
        json::make_field("inputSchema", &Tool::inputSchema, json::FieldPolicy{.raw = true, .object_only = true}),
        json::make_field("title", &Tool::title, json::FieldPolicy{.omit_empty = true}),
        json::make_field("description", &Tool::description, json::FieldPolicy{.omit_empty = true}),
        json::make_field("outputSchema", &Tool::outputSchema, json::FieldPolicy{.raw = true, .omit_empty = true, .object_only = true}),
        json::make_field("annotations", &Tool::annotations, json::FieldPolicy{.raw = true, .omit_empty = true, .object_only = true}),
        json::make_field("icons", &Tool::icons, json::FieldPolicy{.raw = true, .omit_empty = true, .array_only = true}),
        json::make_field("_meta", &Tool::meta, json::FieldPolicy{.raw = true, .omit_empty = true, .object_only = true}));
}
constexpr auto reflect_fields(std::type_identity<Resource>) {
    return std::make_tuple(json::make_field("uri", &Resource::uri), json::make_field("name", &Resource::name),
        json::make_field("title", &Resource::title, json::FieldPolicy{.omit_empty = true}),
        json::make_field("description", &Resource::description, json::FieldPolicy{.omit_empty = true}),
        json::make_field("mimeType", &Resource::mimeType, json::FieldPolicy{.omit_empty = true}),
        json::make_field("size", &Resource::size, json::FieldPolicy{.omit_empty = true, .reject_null = true}),
        json::make_field("annotations", &Resource::annotations, json::FieldPolicy{.raw = true, .omit_empty = true, .object_only = true}),
        json::make_field("icons", &Resource::icons, json::FieldPolicy{.raw = true, .omit_empty = true, .array_only = true}),
        json::make_field("_meta", &Resource::meta, json::FieldPolicy{.raw = true, .omit_empty = true, .object_only = true}));
}
constexpr auto reflect_fields(std::type_identity<PromptArgument>) {
    return std::make_tuple(json::make_field("name", &PromptArgument::name),
        json::make_field("title", &PromptArgument::title, json::FieldPolicy{.omit_empty = true}),
        json::make_field("description", &PromptArgument::description, json::FieldPolicy{.omit_empty = true}),
        json::make_field("required", &PromptArgument::required, json::FieldPolicy{.optional = true, .omit_false = true}));
}
constexpr auto reflect_fields(std::type_identity<Prompt>) {
    return std::make_tuple(json::make_field("name", &Prompt::name),
        json::make_field("title", &Prompt::title, json::FieldPolicy{.omit_empty = true}),
        json::make_field("description", &Prompt::description, json::FieldPolicy{.omit_empty = true}),
        json::make_field("arguments", &Prompt::arguments, json::FieldPolicy{.optional = true, .omit_empty = true}),
        json::make_field("icons", &Prompt::icons, json::FieldPolicy{.raw = true, .omit_empty = true, .array_only = true}),
        json::make_field("_meta", &Prompt::meta, json::FieldPolicy{.raw = true, .omit_empty = true, .object_only = true}));
}
constexpr auto reflect_fields(std::type_identity<SubscriptionFilter>) {
    return std::make_tuple(
        json::make_field("toolsListChanged", &SubscriptionFilter::toolsListChanged, json::FieldPolicy{.optional = true, .omit_false = true}),
        json::make_field("resourcesListChanged", &SubscriptionFilter::resourcesListChanged, json::FieldPolicy{.optional = true, .omit_false = true}),
        json::make_field("promptsListChanged", &SubscriptionFilter::promptsListChanged, json::FieldPolicy{.optional = true, .omit_false = true}),
        json::make_field("resourceSubscriptions", &SubscriptionFilter::resourceSubscriptions, json::FieldPolicy{.optional = true, .omit_empty = true}));
}

struct FeatureCapability {
    bool list_changed = false;
    bool subscribe = false;
};
constexpr auto reflect_fields(std::type_identity<FeatureCapability>) {
    return std::make_tuple(
        json::make_field("listChanged", &FeatureCapability::list_changed, json::FieldPolicy{.optional = true, .omit_false = true}),
        json::make_field("subscribe", &FeatureCapability::subscribe, json::FieldPolicy{.optional = true, .omit_false = true}));
}
struct CapabilityFields {
    std::string extensions;
    std::optional<FeatureCapability> tools, resources, prompts, completions, logging;
};
constexpr auto reflect_fields(std::type_identity<CapabilityFields>) {
    return std::make_tuple(json::make_field("extensions", &CapabilityFields::extensions,
            json::FieldPolicy{.raw = true, .optional = true, .omit_empty = true, .object_only = true}),
        json::make_field("tools", &CapabilityFields::tools, json::FieldPolicy{.omit_empty = true, .reject_null = true}),
        json::make_field("resources", &CapabilityFields::resources, json::FieldPolicy{.omit_empty = true, .reject_null = true}),
        json::make_field("prompts", &CapabilityFields::prompts, json::FieldPolicy{.omit_empty = true, .reject_null = true}),
        json::make_field("completions", &CapabilityFields::completions, json::FieldPolicy{.omit_empty = true, .reject_null = true}),
        json::make_field("logging", &CapabilityFields::logging, json::FieldPolicy{.omit_empty = true, .reject_null = true}));
}
inline CapabilityFields to_wire(const ServerCapabilities& value) {
    CapabilityFields fields;
    fields.extensions = value.extensions;
    if (value.tools) fields.tools = FeatureCapability{value.toolsListChanged, false};
    if (value.resources) fields.resources = FeatureCapability{value.resourcesListChanged, value.resourceSubscriptions};
    if (value.prompts) fields.prompts = FeatureCapability{value.promptsListChanged, false};
    if (value.completions) fields.completions.emplace();
    if (value.logging) fields.logging.emplace();
    return fields;
}
inline auto wire_type(std::type_identity<ServerCapabilities>) { return std::type_identity<CapabilityFields>{}; }
inline json::result<ServerCapabilities> from_wire(std::type_identity<ServerCapabilities>, CapabilityFields fields) {
    ServerCapabilities value;
    value.extensions = std::move(fields.extensions);
    value.tools = fields.tools.has_value();
    value.resources = fields.resources.has_value();
    value.prompts = fields.prompts.has_value();
    value.completions = fields.completions.has_value();
    value.logging = fields.logging.has_value();
    if (fields.tools) value.toolsListChanged = fields.tools->list_changed;
    if (fields.resources) {
        value.resourcesListChanged = fields.resources->list_changed;
        value.resourceSubscriptions = fields.resources->subscribe;
    }
    if (fields.prompts) value.promptsListChanged = fields.prompts->list_changed;
    return value;
}

struct ServerMeta {
    std::optional<Implementation> server_info;
};
constexpr auto reflect_fields(std::type_identity<ServerMeta>) {
    return std::make_tuple(json::make_field("io.modelcontextprotocol/serverInfo", &ServerMeta::server_info, json::FieldPolicy{.omit_empty = true}));
}
struct DiscoverFields {
    std::string result_type{"complete"};
    std::vector<std::string> supported_versions;
    ServerCapabilities capabilities;
    std::optional<std::string> instructions;
    uint64_t ttl_ms = 0;
    std::string cache_scope;
    std::optional<ServerMeta> meta;
};
constexpr auto reflect_fields(std::type_identity<DiscoverFields>) {
    return std::make_tuple(json::make_field("resultType", &DiscoverFields::result_type),
        json::make_field("supportedVersions", &DiscoverFields::supported_versions),
        json::make_field("capabilities", &DiscoverFields::capabilities),
        json::make_field("instructions", &DiscoverFields::instructions, json::FieldPolicy{.omit_empty = true}),
        json::make_field("ttlMs", &DiscoverFields::ttl_ms), json::make_field("cacheScope", &DiscoverFields::cache_scope),
        json::make_field("_meta", &DiscoverFields::meta, json::FieldPolicy{.omit_empty = true, .reject_null = true}));
}
inline DiscoverFields to_wire(const DiscoverResult& value) {
    DiscoverFields fields;
    fields.supported_versions = value.supportedVersions;
    fields.capabilities = value.capabilities;
    fields.instructions = value.instructions;
    fields.ttl_ms = value.ttlMs;
    fields.cache_scope = value.cacheScope == CacheScope::Public ? "public" : "private";
    if (value.serverInfo) fields.meta = ServerMeta{value.serverInfo};
    return fields;
}
inline auto wire_type(std::type_identity<DiscoverResult>) { return std::type_identity<DiscoverFields>{}; }
inline json::result<DiscoverResult> from_wire(std::type_identity<DiscoverResult>, DiscoverFields fields) {
    if (fields.cache_scope != "public" && fields.cache_scope != "private")
        return std::unexpected(std::string("invalid cacheScope"));
    DiscoverResult value;
    value.supportedVersions = std::move(fields.supported_versions);
    value.capabilities = std::move(fields.capabilities);
    value.instructions = std::move(fields.instructions);
    value.ttlMs = fields.ttl_ms;
    value.cacheScope = fields.cache_scope == "public" ? CacheScope::Public : CacheScope::Private;
    if (fields.meta) value.serverInfo = std::move(fields.meta->server_info);
    return value;
}

struct EnvelopeFields {
    std::string version{JSONRPC_VERSION};
    std::optional<RequestId> id;
    std::optional<std::string> method;
    std::optional<std::string> params;
    std::optional<std::string> result;
    std::optional<std::string> error;
};
constexpr auto reflect_fields(std::type_identity<EnvelopeFields>) {
    return std::make_tuple(json::make_field("jsonrpc", &EnvelopeFields::version),
        json::make_field("id", &EnvelopeFields::id, json::FieldPolicy{.omit_empty = true, .reject_null = true}),
        json::make_field("method", &EnvelopeFields::method, json::FieldPolicy{.omit_empty = true}),
        json::make_field("params", &EnvelopeFields::params, json::FieldPolicy{.raw = true, .omit_empty = true}),
        json::make_field("result", &EnvelopeFields::result, json::FieldPolicy{.raw = true, .omit_empty = true}),
        json::make_field("error", &EnvelopeFields::error, json::FieldPolicy{.raw = true, .omit_empty = true}));
}
inline EnvelopeFields to_wire(const JsonRpcRequest& value) {
    return {JSONRPC_VERSION, value.id, value.method, value.params, {}, {}};
}
struct ToolResultFields {
    std::string result_type{"complete"};
    std::optional<ServerMeta> meta;
    std::vector<std::string> content;
    std::optional<std::string> structured_content;
    bool is_error = false;
};
constexpr auto reflect_fields(std::type_identity<ToolResultFields>) {
    return std::make_tuple(json::make_field("resultType", &ToolResultFields::result_type),
        json::make_field("_meta", &ToolResultFields::meta, json::FieldPolicy{.omit_empty = true}),
        json::make_field("content", &ToolResultFields::content, json::FieldPolicy{.raw = true, .object_only = true}),
        json::make_field("structuredContent", &ToolResultFields::structured_content, json::FieldPolicy{.raw = true, .omit_empty = true}),
        json::make_field("isError", &ToolResultFields::is_error, json::FieldPolicy{.optional = true, .omit_false = true}));
}
struct ResourceResultFields {
    std::string result_type{"complete"};
    std::optional<ServerMeta> meta;
    std::vector<std::string> contents;
    uint64_t ttl_ms = 0;
    std::string cache_scope;
};
constexpr auto reflect_fields(std::type_identity<ResourceResultFields>) {
    return std::make_tuple(json::make_field("resultType", &ResourceResultFields::result_type),
        json::make_field("_meta", &ResourceResultFields::meta, json::FieldPolicy{.omit_empty = true}),
        json::make_field("contents", &ResourceResultFields::contents, json::FieldPolicy{.raw = true, .object_only = true}),
        json::make_field("ttlMs", &ResourceResultFields::ttl_ms),
        json::make_field("cacheScope", &ResourceResultFields::cache_scope));
}
struct PromptResultFields {
    std::string result_type{"complete"};
    std::optional<ServerMeta> meta;
    std::vector<std::string> messages;
    std::optional<std::string> description;
};
constexpr auto reflect_fields(std::type_identity<PromptResultFields>) {
    return std::make_tuple(json::make_field("resultType", &PromptResultFields::result_type),
        json::make_field("_meta", &PromptResultFields::meta, json::FieldPolicy{.omit_empty = true}),
        json::make_field("messages", &PromptResultFields::messages, json::FieldPolicy{.raw = true, .object_only = true}),
        json::make_field("description", &PromptResultFields::description, json::FieldPolicy{.omit_empty = true}));
}
struct SubscriptionMeta {
    RequestId id;
};
constexpr auto reflect_fields(std::type_identity<SubscriptionMeta>) {
    return std::make_tuple(json::make_field("io.modelcontextprotocol/subscriptionId", &SubscriptionMeta::id));
}
struct SubscriptionParams {
    SubscriptionMeta meta;
    std::optional<SubscriptionFilter> notifications;
    std::optional<std::string> uri;
};
constexpr auto reflect_fields(std::type_identity<SubscriptionParams>) {
    return std::make_tuple(json::make_field("_meta", &SubscriptionParams::meta),
        json::make_field("notifications", &SubscriptionParams::notifications, json::FieldPolicy{.omit_empty = true}),
        json::make_field("uri", &SubscriptionParams::uri, json::FieldPolicy{.omit_empty = true}));
}
struct TextResource {
    std::string uri;
    std::string text;
    std::optional<std::string> mime_type;
};
constexpr auto reflect_fields(std::type_identity<TextResource>) {
    return std::make_tuple(json::make_field("uri", &TextResource::uri), json::make_field("text", &TextResource::text),
        json::make_field("mimeType", &TextResource::mime_type, json::FieldPolicy{.omit_empty = true}));
}
struct TextContent {
    std::string type{"text"};
    std::string text;
};
constexpr auto reflect_fields(std::type_identity<TextContent>) {
    return std::make_tuple(json::make_field("type", &TextContent::type), json::make_field("text", &TextContent::text));
}
} // namespace galay::mcp::v2
#endif
