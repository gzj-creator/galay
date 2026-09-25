#include "protocol.h"

#include <utility>

namespace galay::mcp::v2 {

namespace {

constexpr const char* kProtocolVersionKey = "io.modelcontextprotocol/protocolVersion";
constexpr const char* kClientCapabilitiesKey = "io.modelcontextprotocol/clientCapabilities";
constexpr const char* kClientInfoKey = "io.modelcontextprotocol/clientInfo";
constexpr const char* kServerInfoKey = "io.modelcontextprotocol/serverInfo";
constexpr const char* kLogLevelKey = "io.modelcontextprotocol/logLevel";

void writeRequestId(json::stream::StreamWriter& writer, const RequestId& id)
{
    // StreamWriter 失败粘滞：中间结果统一丢弃，由 finish() 统一检查
    if (const auto* number = std::get_if<int64_t>(&id)) {
        (void)writer.number(*number);
    } else {
        (void)writer.string(std::get<std::string>(id));
    }
}

std::expected<RequestId, McpError> parseRequestId(const json::Json& element)
{
    if (element.as_int64().has_value()) {
        return RequestId{element.as_int64().value()};
    }
    if (element.is_string()) {
        return RequestId{std::string(element.as_string().value())};
    }
    return std::unexpected(McpError::invalidRequest("id must be a string or integer"));
}

std::expected<json::Json, McpError> requireObject(const json::Json& element,
                                                   std::string_view context)
{
    if (!element.is_object()) {
        return std::unexpected(McpError::invalidParams(
            std::string(context) + " must be an object"));
    }
    return element;
}

std::expected<std::string, McpError> requireString(const json::Json& object,
                                                    const char* key)
{
    auto value = object.at(key).as_string();
    if (!value) {
        return std::unexpected(McpError::invalidParams(
            std::string("missing or invalid ") + key));
    }
    return std::string(*value);
}

void writeImplementationMeta(json::stream::StreamWriter& writer,
                             const std::optional<Implementation>& implementation,
                             const char* key)
{
    // StreamWriter 失败粘滞：中间结果统一丢弃，由 finish() 统一检查
    if (!implementation) {
        return;
    }
    (void)writer.key("_meta");
    (void)writer.start_object();
    (void)writer.key(key);
    (void)writer.raw(implementation->toJson());
    (void)writer.end_object();
}

void writeCacheFields(json::stream::StreamWriter& writer, uint64_t ttlMs, CacheScope scope)
{
    // StreamWriter 失败粘滞：中间结果统一丢弃，由 finish() 统一检查
    (void)writer.key("resultType");
    (void)writer.string("complete");
    (void)writer.key("ttlMs");
    (void)writer.number(ttlMs);
    (void)writer.key("cacheScope");
    (void)writer.string(scope == CacheScope::Public ? "public" : "private");
}

void writeOptionalString(json::stream::StreamWriter& writer,
                         const char* key,
                         const std::optional<std::string>& value)
{
    // StreamWriter 失败粘滞：中间结果统一丢弃，由 finish() 统一检查
    if (!value) {
        return;
    }
    (void)writer.key(key);
    (void)writer.string(*value);
}

void writeOptionalRaw(json::stream::StreamWriter& writer,
                      const char* key,
                      const std::optional<std::string>& value)
{
    // StreamWriter 失败粘滞：中间结果统一丢弃，由 finish() 统一检查
    if (!value) {
        return;
    }
    (void)writer.key(key);
    (void)writer.raw(*value);
}

std::expected<std::string, McpError> rawJson(const json::Json& element,
                                             std::string_view context)
{
    std::string raw;
    auto serialized = json::stream::serialize(
        element, [&](std::string_view chunk) -> json::result<void> {
            raw.append(chunk);
            return {};
        });
    if (!serialized) {
        return std::unexpected(McpError::invalidParams(
            std::string("invalid ") + std::string(context)));
    }
    return raw;
}

std::expected<std::string, McpError> requireRawObject(const json::Json& object,
                                                      const char* key)
{
    const json::Json element = object.at(key);
    if (!element.valid() || !element.is_object()) {
        return std::unexpected(McpError::invalidParams(
            std::string("missing or invalid ") + key));
    }
    return rawJson(element, key);
}

std::expected<std::optional<std::string>, McpError> optionalRawObject(
    const json::Json& object,
    const char* key)
{
    const json::Json element = object.at(key);
    if (!element.valid()) {
        return std::optional<std::string>{};
    }
    if (!element.is_object()) {
        return std::unexpected(McpError::invalidParams(
            std::string("invalid ") + key));
    }
    auto raw = rawJson(element, key);
    if (!raw) {
        return std::unexpected(raw.error());
    }
    return std::optional<std::string>{std::move(raw.value())};
}

std::expected<std::optional<std::string>, McpError> optionalRawArray(
    const json::Json& object,
    const char* key)
{
    const json::Json element = object.at(key);
    if (!element.valid()) {
        return std::optional<std::string>{};
    }
    if (!element.is_array()) {
        return std::unexpected(McpError::invalidParams(
            std::string("invalid ") + key));
    }
    auto raw = rawJson(element, key);
    if (!raw) {
        return std::unexpected(raw.error());
    }
    return std::optional<std::string>{std::move(raw.value())};
}

std::expected<uint64_t, McpError> requireUint64(const json::Json& object,
                                                const char* key)
{
    const json::Json element = object.at(key);
    if (!element.valid()) {
        return std::unexpected(McpError::invalidParams(
            std::string("missing or invalid ") + key));
    }
    auto unsignedValue = element.as_uint64();
    if (unsignedValue.has_value()) {
        return unsignedValue.value();
    }
    auto signedValue = element.as_int64();
    if (signedValue.has_value() && signedValue.value() >= 0) {
        return static_cast<uint64_t>(signedValue.value());
    }
    return std::unexpected(McpError::invalidParams(
        std::string("missing or invalid ") + key));
}

std::expected<std::optional<uint64_t>, McpError> optionalUint64(
    const json::Json& object,
    const char* key)
{
    const json::Json element = object.at(key);
    if (!element.valid()) {
        return std::optional<uint64_t>{};
    }
    auto unsignedValue = element.as_uint64();
    if (unsignedValue.has_value()) {
        return std::optional<uint64_t>{unsignedValue.value()};
    }
    auto signedValue = element.as_int64();
    if (signedValue.has_value() && signedValue.value() >= 0) {
        return std::optional<uint64_t>{static_cast<uint64_t>(signedValue.value())};
    }
    return std::unexpected(McpError::invalidParams(
        std::string("invalid ") + key));
}

std::expected<CacheScope, McpError> requireCacheScope(const json::Json& object)
{
    auto value = object.at("cacheScope").as_string();
    if (!value) {
        return std::unexpected(McpError::invalidParams("missing or invalid cacheScope"));
    }
    if (*value == "public") {
        return CacheScope::Public;
    }
    if (*value == "private") {
        return CacheScope::Private;
    }
    return std::unexpected(McpError::invalidParams("invalid cacheScope"));
}

std::expected<ResultType, McpError> requireResultType(const json::Json& object,
                                                      std::string& typeName)
{
    auto value = object.at("resultType").as_string();
    if (!value) {
        return std::unexpected(McpError::invalidResponse("missing resultType"));
    }
    typeName = std::string(*value);
    if (typeName == "complete") {
        return ResultType::Complete;
    }
    if (typeName == "input_required") {
        return ResultType::InputRequired;
    }
    return ResultType::Extension;
}

std::expected<std::optional<Implementation>, McpError> parseServerInfo(
    const json::Json& object)
{
    const json::Json metaElement = object.at("_meta");
    if (!metaElement.valid()) {
        return std::optional<Implementation>{};
    }
    if (!metaElement.is_object()) {
        return std::unexpected(McpError::invalidParams("invalid _meta"));
    }
    const json::Json serverInfoElement = metaElement.at(kServerInfoKey);
    if (!serverInfoElement.valid()) {
        return std::optional<Implementation>{};
    }
    auto serverInfo = Implementation::fromJson(serverInfoElement);
    if (!serverInfo) {
        return std::unexpected(serverInfo.error());
    }
    return std::optional<Implementation>{std::move(serverInfo.value())};
}

std::expected<void, McpError> parseCacheFields(const json::Json& object,
                                               uint64_t& ttlMs,
                                               CacheScope& cacheScope)
{
    std::string unusedTypeName;
    auto resultType = requireResultType(object, unusedTypeName);
    if (!resultType) {
        return std::unexpected(resultType.error());
    }
    auto ttl = requireUint64(object, "ttlMs");
    if (!ttl) {
        return std::unexpected(ttl.error());
    }
    auto scope = requireCacheScope(object);
    if (!scope) {
        return std::unexpected(scope.error());
    }
    ttlMs = ttl.value();
    cacheScope = scope.value();
    return {};
}

std::expected<void, McpError> parseFeatureCapability(const json::Json& object,
                                                     const char* key,
                                                     bool& present,
                                                     bool* listChanged,
                                                     bool* subscribe)
{
    const json::Json element = object.at(key);
    if (!element.valid()) {
        return {};
    }
    if (!element.is_object()) {
        return std::unexpected(McpError::invalidParams(
            std::string("invalid capability ") + key));
    }
    present = true;
    if (listChanged != nullptr) {
        auto value = element.at("listChanged").as_bool();
        if (value) {
            *listChanged = *value;
        }
    }
    if (subscribe != nullptr) {
        auto value = element.at("subscribe").as_bool();
        if (value) {
            *subscribe = *value;
        }
    }
    return {};
}

std::expected<std::vector<std::string>, McpError> requireStringArray(
    const json::Json& object,
    const char* key)
{
    const json::Json element = object.at(key);
    if (!element.valid() || !element.is_array()) {
        return std::unexpected(McpError::invalidParams(
            std::string("missing or invalid ") + key));
    }
    std::vector<std::string> values;
    for (size_t i = 0; i < element.size(); ++i) {
        const json::Json item = element.at(i);
        auto value = item.as_string();
        if (!value) {
            return std::unexpected(McpError::invalidParams(
                std::string("invalid ") + key));
        }
        values.push_back(std::string(*value));
    }
    return values;
}

std::expected<std::vector<PromptArgument>, McpError> parsePromptArguments(
    const json::Json& object)
{
    const json::Json element = object.at("arguments");
    if (!element.valid()) {
        return std::vector<PromptArgument>{};
    }
    if (!element.is_array()) {
        return std::unexpected(McpError::invalidParams("invalid arguments"));
    }
    std::vector<PromptArgument> arguments;
    for (size_t i = 0; i < element.size(); ++i) {
        const json::Json item = element.at(i);
        auto argument = PromptArgument::fromJson(item);
        if (!argument) {
            return std::unexpected(argument.error());
        }
        arguments.push_back(std::move(argument.value()));
    }
    return arguments;
}

} // namespace

std::string Implementation::toJson() const
{
    std::string out;
    auto writer = makeJsonWriter(out);
    // StreamWriter 失败粘滞：中间结果统一丢弃，由 finish() 统一检查
    (void)writer.start_object();
    (void)writer.key("name");
    (void)writer.string(name);
    (void)writer.key("version");
    (void)writer.string(version);
    if (title) {
        (void)writer.key("title");
        (void)writer.string(*title);
    }
    if (description) {
        (void)writer.key("description");
        (void)writer.string(*description);
    }
    if (websiteUrl) {
        (void)writer.key("websiteUrl");
        (void)writer.string(*websiteUrl);
    }
    if (!icons.empty()) {
        (void)writer.key("icons");
        (void)writer.raw(icons);
    }
    (void)writer.end_object();
    if (!writer.finish()) {
        return std::string{};
    }
    return std::move(out);
}

std::expected<Implementation, McpError> Implementation::fromJson(const json::Json& element)
{
    auto objectResult = requireObject(element, "implementation");
    if (!objectResult) {
        return std::unexpected(objectResult.error());
    }
    const json::Json object = objectResult.value();
    auto name = requireString(object, "name");
    auto version = requireString(object, "version");
    if (!name) {
        return std::unexpected(name.error());
    }
    if (!version) {
        return std::unexpected(version.error());
    }

    Implementation implementation;
    implementation.name = std::move(name.value());
    implementation.version = std::move(version.value());
    if (auto value = object.at("title").as_string()) {
        implementation.title = std::string(*value);
    }
    if (auto value = object.at("description").as_string()) {
        implementation.description = std::string(*value);
    }
    if (auto value = object.at("websiteUrl").as_string()) {
        implementation.websiteUrl = std::string(*value);
    }
    const json::Json iconsElement = object.at("icons");
    if (iconsElement.valid()) {
        auto raw = rawJson(iconsElement, "icons");
        if (!raw) {
            return std::unexpected(McpError::invalidParams("invalid icons"));
        }
        implementation.icons = std::move(raw.value());
    }
    return implementation;
}

std::string RequestMeta::toJson() const
{
    std::string out;
    auto writer = makeJsonWriter(out);
    // StreamWriter 失败粘滞：中间结果统一丢弃，由 finish() 统一检查
    (void)writer.start_object();
    if (progressToken) {
        (void)writer.key("progressToken");
        writeRequestId(writer, *progressToken);
    }
    (void)writer.key(kProtocolVersionKey);
    (void)writer.string(protocolVersion);
    if (clientInfo) {
        (void)writer.key(kClientInfoKey);
        (void)writer.raw(clientInfo->toJson());
    }
    (void)writer.key(kClientCapabilitiesKey);
    (void)writer.raw(clientCapabilities.empty() ? "{}" : clientCapabilities);
    if (logLevel) {
        (void)writer.key(kLogLevelKey);
        (void)writer.string(*logLevel);
    }
    (void)writer.end_object();
    if (!writer.finish()) {
        return std::string{};
    }
    return std::move(out);
}

std::expected<RequestMeta, McpError> RequestMeta::fromJson(const json::Json& element)
{
    auto objectResult = requireObject(element, "_meta");
    if (!objectResult) {
        return std::unexpected(objectResult.error());
    }
    const json::Json object = objectResult.value();

    RequestMeta meta;
    auto protocolVersion = requireString(object, kProtocolVersionKey);
    if (!protocolVersion) {
        return std::unexpected(protocolVersion.error());
    }
    meta.protocolVersion = std::move(protocolVersion.value());

    const json::Json capabilitiesElement = object.at(kClientCapabilitiesKey);
    auto capabilitiesRaw = rawJson(capabilitiesElement, kClientCapabilitiesKey);
    if (!capabilitiesElement.is_object() || !capabilitiesRaw) {
        return std::unexpected(McpError::invalidParams(
            "missing or invalid io.modelcontextprotocol/clientCapabilities"));
    }
    meta.clientCapabilities = std::move(capabilitiesRaw.value());

    const json::Json clientInfoElement = object.at(kClientInfoKey);
    if (clientInfoElement.valid()) {
        auto clientInfo = Implementation::fromJson(clientInfoElement);
        if (!clientInfo) {
            return std::unexpected(clientInfo.error());
        }
        meta.clientInfo = std::move(clientInfo.value());
    }

    if (auto logLevel = object.at(kLogLevelKey).as_string()) {
        meta.logLevel = std::string(*logLevel);
    }

    const json::Json progressToken = object.at("progressToken");
    if (progressToken.valid()) {
        auto parsed = parseRequestId(progressToken);
        if (!parsed) {
            return std::unexpected(McpError::invalidParams("invalid progressToken"));
        }
        meta.progressToken = std::move(parsed.value());
    }
    return meta;
}

std::string ServerCapabilities::toJson() const
{
    std::string out;
    auto writer = makeJsonWriter(out);
    // StreamWriter 失败粘滞：中间结果统一丢弃，由 finish() 统一检查
    (void)writer.start_object();
    if (!extensions.empty()) {
        (void)writer.key("extensions");
        (void)writer.raw(extensions);
    }
    if (tools) {
        (void)writer.key("tools");
        (void)writer.start_object();
        if (toolsListChanged) {
            (void)writer.key("listChanged");
            (void)writer.boolean(true);
        }
        (void)writer.end_object();
    }
    if (resources) {
        (void)writer.key("resources");
        (void)writer.start_object();
        if (resourceSubscriptions) {
            (void)writer.key("subscribe");
            (void)writer.boolean(true);
        }
        if (resourcesListChanged) {
            (void)writer.key("listChanged");
            (void)writer.boolean(true);
        }
        (void)writer.end_object();
    }
    if (prompts) {
        (void)writer.key("prompts");
        (void)writer.start_object();
        if (promptsListChanged) {
            (void)writer.key("listChanged");
            (void)writer.boolean(true);
        }
        (void)writer.end_object();
    }
    if (completions) {
        (void)writer.key("completions");
        (void)writer.start_object();
        (void)writer.end_object();
    }
    if (logging) {
        (void)writer.key("logging");
        (void)writer.start_object();
        (void)writer.end_object();
    }
    (void)writer.end_object();
    if (!writer.finish()) {
        return std::string{};
    }
    return std::move(out);
}

std::expected<ServerCapabilities, McpError> ServerCapabilities::fromJson(
    const json::Json& element)
{
    auto objectResult = requireObject(element, "capabilities");
    if (!objectResult) {
        return std::unexpected(objectResult.error());
    }
    const json::Json object = objectResult.value();
    ServerCapabilities capabilities;

    auto extensions = optionalRawObject(object, "extensions");
    if (!extensions) {
        return std::unexpected(extensions.error());
    }
    capabilities.extensions = std::move(extensions.value().value_or(std::string{}));

    auto tools = parseFeatureCapability(object,
                                        "tools",
                                        capabilities.tools,
                                        &capabilities.toolsListChanged,
                                        nullptr);
    if (!tools) {
        return std::unexpected(tools.error());
    }
    auto resources = parseFeatureCapability(object,
                                            "resources",
                                            capabilities.resources,
                                            &capabilities.resourcesListChanged,
                                            &capabilities.resourceSubscriptions);
    if (!resources) {
        return std::unexpected(resources.error());
    }
    auto prompts = parseFeatureCapability(object,
                                          "prompts",
                                          capabilities.prompts,
                                          &capabilities.promptsListChanged,
                                          nullptr);
    if (!prompts) {
        return std::unexpected(prompts.error());
    }
    auto completions = parseFeatureCapability(object,
                                             "completions",
                                             capabilities.completions,
                                             nullptr,
                                             nullptr);
    if (!completions) {
        return std::unexpected(completions.error());
    }
    auto logging = parseFeatureCapability(object,
                                         "logging",
                                         capabilities.logging,
                                         nullptr,
                                         nullptr);
    if (!logging) {
        return std::unexpected(logging.error());
    }
    return capabilities;
}

std::string Tool::toJson() const
{
    std::string out;
    auto writer = makeJsonWriter(out);
    // StreamWriter 失败粘滞：中间结果统一丢弃，由 finish() 统一检查
    (void)writer.start_object();
    (void)writer.key("name");
    (void)writer.string(name);
    writeOptionalString(writer, "title", title);
    writeOptionalString(writer, "description", description);
    (void)writer.key("inputSchema");
    (void)writer.raw(inputSchema.empty() ? "{\"type\":\"object\"}" : inputSchema);
    writeOptionalRaw(writer, "outputSchema", outputSchema);
    writeOptionalRaw(writer, "annotations", annotations);
    writeOptionalRaw(writer, "icons", icons);
    writeOptionalRaw(writer, "_meta", meta);
    (void)writer.end_object();
    if (!writer.finish()) {
        return std::string{};
    }
    return std::move(out);
}

std::expected<Tool, McpError> Tool::fromJson(const json::Json& element)
{
    auto objectResult = requireObject(element, "tool");
    if (!objectResult) {
        return std::unexpected(objectResult.error());
    }
    const json::Json object = objectResult.value();
    auto name = requireString(object, "name");
    if (!name) {
        return std::unexpected(name.error());
    }
    auto inputSchema = requireRawObject(object, "inputSchema");
    if (!inputSchema) {
        return std::unexpected(inputSchema.error());
    }
    const json::Json inputElement = object.at("inputSchema");
    const auto type = inputElement.at("type").as_string();
    if (!inputElement.is_object() || !type || *type != "object") {
        return std::unexpected(McpError::invalidParams(
            "tool inputSchema root type must be object"));
    }

    Tool tool;
    tool.name = std::move(name.value());
    tool.inputSchema = std::move(inputSchema.value());
    if (auto value = object.at("title").as_string()) {
        tool.title = std::string(*value);
    }
    if (auto value = object.at("description").as_string()) {
        tool.description = std::string(*value);
    }
    auto outputSchema = optionalRawObject(object, "outputSchema");
    if (!outputSchema) {
        return std::unexpected(outputSchema.error());
    }
    tool.outputSchema = std::move(outputSchema.value());
    auto annotations = optionalRawObject(object, "annotations");
    if (!annotations) {
        return std::unexpected(annotations.error());
    }
    tool.annotations = std::move(annotations.value());
    auto icons = optionalRawArray(object, "icons");
    if (!icons) {
        return std::unexpected(icons.error());
    }
    tool.icons = std::move(icons.value());
    auto meta = optionalRawObject(object, "_meta");
    if (!meta) {
        return std::unexpected(meta.error());
    }
    tool.meta = std::move(meta.value());
    return tool;
}

std::string Resource::toJson() const
{
    std::string out;
    auto writer = makeJsonWriter(out);
    // StreamWriter 失败粘滞：中间结果统一丢弃，由 finish() 统一检查
    (void)writer.start_object();
    (void)writer.key("uri");
    (void)writer.string(uri);
    (void)writer.key("name");
    (void)writer.string(name);
    writeOptionalString(writer, "title", title);
    writeOptionalString(writer, "description", description);
    writeOptionalString(writer, "mimeType", mimeType);
    if (size) {
        (void)writer.key("size");
        (void)writer.number(*size);
    }
    writeOptionalRaw(writer, "annotations", annotations);
    writeOptionalRaw(writer, "icons", icons);
    writeOptionalRaw(writer, "_meta", meta);
    (void)writer.end_object();
    if (!writer.finish()) {
        return std::string{};
    }
    return std::move(out);
}

std::expected<Resource, McpError> Resource::fromJson(const json::Json& element)
{
    auto objectResult = requireObject(element, "resource");
    if (!objectResult) {
        return std::unexpected(objectResult.error());
    }
    const json::Json object = objectResult.value();
    auto uri = requireString(object, "uri");
    auto name = requireString(object, "name");
    if (!uri) {
        return std::unexpected(uri.error());
    }
    if (!name) {
        return std::unexpected(name.error());
    }
    Resource resource;
    resource.uri = std::move(uri.value());
    resource.name = std::move(name.value());
    if (auto value = object.at("title").as_string()) {
        resource.title = std::string(*value);
    }
    if (auto value = object.at("description").as_string()) {
        resource.description = std::string(*value);
    }
    if (auto value = object.at("mimeType").as_string()) {
        resource.mimeType = std::string(*value);
    }
    auto size = optionalUint64(object, "size");
    if (!size) {
        return std::unexpected(size.error());
    }
    resource.size = size.value();
    auto annotations = optionalRawObject(object, "annotations");
    if (!annotations) {
        return std::unexpected(annotations.error());
    }
    resource.annotations = std::move(annotations.value());
    auto icons = optionalRawArray(object, "icons");
    if (!icons) {
        return std::unexpected(icons.error());
    }
    resource.icons = std::move(icons.value());
    auto meta = optionalRawObject(object, "_meta");
    if (!meta) {
        return std::unexpected(meta.error());
    }
    resource.meta = std::move(meta.value());
    return resource;
}

std::string PromptArgument::toJson() const
{
    std::string out;
    auto writer = makeJsonWriter(out);
    // StreamWriter 失败粘滞：中间结果统一丢弃，由 finish() 统一检查
    (void)writer.start_object();
    (void)writer.key("name");
    (void)writer.string(name);
    writeOptionalString(writer, "title", title);
    writeOptionalString(writer, "description", description);
    if (required) {
        (void)writer.key("required");
        (void)writer.boolean(true);
    }
    (void)writer.end_object();
    if (!writer.finish()) {
        return std::string{};
    }
    return std::move(out);
}

std::expected<PromptArgument, McpError> PromptArgument::fromJson(
    const json::Json& element)
{
    auto objectResult = requireObject(element, "prompt argument");
    if (!objectResult) {
        return std::unexpected(objectResult.error());
    }
    const json::Json object = objectResult.value();
    auto name = requireString(object, "name");
    if (!name) {
        return std::unexpected(name.error());
    }
    PromptArgument argument;
    argument.name = std::move(name.value());
    if (auto value = object.at("title").as_string()) {
        argument.title = std::string(*value);
    }
    if (auto value = object.at("description").as_string()) {
        argument.description = std::string(*value);
    }
    if (auto value = object.at("required").as_bool()) {
        argument.required = *value;
    }
    return argument;
}

std::string Prompt::toJson() const
{
    std::string out;
    auto writer = makeJsonWriter(out);
    // StreamWriter 失败粘滞：中间结果统一丢弃，由 finish() 统一检查
    (void)writer.start_object();
    (void)writer.key("name");
    (void)writer.string(name);
    writeOptionalString(writer, "title", title);
    writeOptionalString(writer, "description", description);
    if (!arguments.empty()) {
        (void)writer.key("arguments");
        (void)writer.start_array();
        for (const auto& argument : arguments) {
            (void)writer.raw(argument.toJson());
        }
        (void)writer.end_array();
    }
    writeOptionalRaw(writer, "icons", icons);
    writeOptionalRaw(writer, "_meta", meta);
    (void)writer.end_object();
    if (!writer.finish()) {
        return std::string{};
    }
    return std::move(out);
}

std::expected<Prompt, McpError> Prompt::fromJson(const json::Json& element)
{
    auto objectResult = requireObject(element, "prompt");
    if (!objectResult) {
        return std::unexpected(objectResult.error());
    }
    const json::Json object = objectResult.value();
    auto name = requireString(object, "name");
    if (!name) {
        return std::unexpected(name.error());
    }
    Prompt prompt;
    prompt.name = std::move(name.value());
    if (auto value = object.at("title").as_string()) {
        prompt.title = std::string(*value);
    }
    if (auto value = object.at("description").as_string()) {
        prompt.description = std::string(*value);
    }
    auto arguments = parsePromptArguments(object);
    if (!arguments) {
        return std::unexpected(arguments.error());
    }
    prompt.arguments = std::move(arguments.value());
    auto icons = optionalRawArray(object, "icons");
    if (!icons) {
        return std::unexpected(icons.error());
    }
    prompt.icons = std::move(icons.value());
    auto meta = optionalRawObject(object, "_meta");
    if (!meta) {
        return std::unexpected(meta.error());
    }
    prompt.meta = std::move(meta.value());
    return prompt;
}

std::string DiscoverResult::toJson() const
{
    std::string out;
    auto writer = makeJsonWriter(out);
    // StreamWriter 失败粘滞：中间结果统一丢弃，由 finish() 统一检查
    (void)writer.start_object();
    writeCacheFields(writer, ttlMs, cacheScope);
    writeImplementationMeta(writer, serverInfo, kServerInfoKey);
    (void)writer.key("supportedVersions");
    (void)writer.start_array();
    for (const auto& version : supportedVersions) {
        (void)writer.string(version);
    }
    (void)writer.end_array();
    (void)writer.key("capabilities");
    (void)writer.raw(capabilities.toJson());
    if (instructions) {
        (void)writer.key("instructions");
        (void)writer.string(*instructions);
    }
    (void)writer.end_object();
    if (!writer.finish()) {
        return std::string{};
    }
    return std::move(out);
}

std::expected<DiscoverResult, McpError> DiscoverResult::fromJson(
    const json::Json& element)
{
    auto objectResult = requireObject(element, "discover result");
    if (!objectResult) {
        return std::unexpected(objectResult.error());
    }
    const json::Json object = objectResult.value();
    DiscoverResult result;
    auto cache = parseCacheFields(object, result.ttlMs, result.cacheScope);
    if (!cache) {
        return std::unexpected(cache.error());
    }
    auto supportedVersions = requireStringArray(object, "supportedVersions");
    if (!supportedVersions) {
        return std::unexpected(supportedVersions.error());
    }
    result.supportedVersions = std::move(supportedVersions.value());
    const json::Json capabilitiesElement = object.at("capabilities");
    if (!capabilitiesElement.valid()) {
        return std::unexpected(McpError::invalidParams("missing capabilities"));
    }
    auto capabilities = ServerCapabilities::fromJson(capabilitiesElement);
    if (!capabilities) {
        return std::unexpected(capabilities.error());
    }
    result.capabilities = std::move(capabilities.value());
    if (auto instructions = object.at("instructions").as_string()) {
        result.instructions = std::string(*instructions);
    }
    auto serverInfo = parseServerInfo(object);
    if (!serverInfo) {
        return std::unexpected(serverInfo.error());
    }
    result.serverInfo = std::move(serverInfo.value());
    return result;
}

std::string ListResult::toJson() const
{
    std::string out;
    auto writer = makeJsonWriter(out);
    // StreamWriter 失败粘滞：中间结果统一丢弃，由 finish() 统一检查
    (void)writer.start_object();
    writeCacheFields(writer, ttlMs, cacheScope);
    writeImplementationMeta(writer, serverInfo, kServerInfoKey);
    (void)writer.key(field);
    (void)writer.start_array();
    for (const auto& item : items) {
        (void)writer.raw(item);
    }
    (void)writer.end_array();
    if (nextCursor) {
        (void)writer.key("nextCursor");
        (void)writer.string(*nextCursor);
    }
    (void)writer.end_object();
    if (!writer.finish()) {
        return std::string{};
    }
    return std::move(out);
}

std::string JsonRpcRequest::toJson() const
{
    std::string out;
    auto writer = makeJsonWriter(out);
    // StreamWriter 失败粘滞：中间结果统一丢弃，由 finish() 统一检查
    (void)writer.start_object();
    (void)writer.key("jsonrpc");
    (void)writer.string(JSONRPC_VERSION);
    (void)writer.key("id");
    writeRequestId(writer, id);
    (void)writer.key("method");
    (void)writer.string(method);
    if (params) {
        (void)writer.key("params");
        (void)writer.raw(*params);
    }
    (void)writer.end_object();
    if (!writer.finish()) {
        return std::string{};
    }
    return std::move(out);
}

std::string makeRequestParams(const RequestMeta& meta)
{
    std::string out;
    auto writer = makeJsonWriter(out);
    // StreamWriter 失败粘滞：中间结果统一丢弃，由 finish() 统一检查
    (void)writer.start_object();
    (void)writer.key("_meta");
    (void)writer.raw(meta.toJson());
    (void)writer.end_object();
    if (!writer.finish()) {
        return std::string{};
    }
    return std::move(out);
}

std::expected<std::string, McpError> makeRequestParams(const RequestMeta& meta,
                                                       std::string_view fieldsJson)
{
    auto document = JsonDocument::parse(fieldsJson);
    if (!document) {
        return std::unexpected(document.error());
    }
    if (!document->root().is_object()) {
        return std::unexpected(McpError::invalidParams("request fields must be an object"));
    }
    const json::Json fields = document->root();

    std::string out;
    auto writer = makeJsonWriter(out);
    // StreamWriter 失败粘滞：中间结果统一丢弃，由 finish() 统一检查
    std::optional<McpError> failure;
    (void)writer.start_object();
    fields.for_each_member([&](std::string_view fieldKey, const json::Json& value) -> json::result<void> {
        const std::string key(fieldKey);
        if (key == "_meta") {
            failure = McpError::invalidParams(
                "request fields must not contain _meta");
            return std::unexpected(std::string("stop"));
        }
        auto raw = rawJson(value, key);
        if (!raw) {
            failure = raw.error();
            return std::unexpected(std::string("stop"));
        }
        (void)writer.key(key);
        (void)writer.raw(raw.value());
        return {};
    });
    if (failure) {
        return std::unexpected(*failure);
    }
    (void)writer.key("_meta");
    (void)writer.raw(meta.toJson());
    (void)writer.end_object();
    auto finished = writer.finish();
    if (!finished) {
        return std::unexpected(McpError::invalidParams(
            "failed to encode JSON: " + finished.error()));
    }
    return std::move(out);
}

std::expected<ParsedRequest, McpError> parseRequest(std::string_view body)
{
    auto document = JsonDocument::parse(body);
    if (!document) {
        return std::unexpected(document.error());
    }

    ParsedRequest parsed;
    parsed.document = std::move(document.value());
    const json::Json object = parsed.document.root();
    if (!object.is_object()) {
        return std::unexpected(McpError::invalidRequest("request must be an object"));
    }
    const auto jsonrpc = object.at("jsonrpc").as_string();
    if (!jsonrpc || *jsonrpc != JSONRPC_VERSION) {
        return std::unexpected(McpError::invalidRequest("missing or invalid jsonrpc"));
    }
    auto method = requireString(object, "method");
    if (!method) {
        return std::unexpected(McpError::invalidRequest(method.error().details()));
    }
    parsed.request.method = std::move(method.value());

    const json::Json idElement = object.at("id");
    if (!idElement.valid()) {
        return std::unexpected(McpError::invalidRequest("missing id"));
    }
    auto id = parseRequestId(idElement);
    if (!id) {
        return std::unexpected(id.error());
    }
    parsed.request.id = std::move(id.value());

    parsed.request.params = object.at("params");
    if (!parsed.request.params.valid()) {
        return std::unexpected(McpError::invalidParams("missing params"));
    }
    if (!parsed.request.params.is_object()) {
        return std::unexpected(McpError::invalidParams("params must be an object"));
    }
    const json::Json metaElement = parsed.request.params.at("_meta");
    if (!metaElement.valid()) {
        return std::unexpected(McpError::invalidParams("missing _meta"));
    }
    auto meta = RequestMeta::fromJson(metaElement);
    if (!meta) {
        return std::unexpected(meta.error());
    }
    parsed.request.meta = std::move(meta.value());
    return parsed;
}

std::expected<ParsedResult, McpError> parseResult(std::string_view body)
{
    auto document = JsonDocument::parse(body);
    if (!document) {
        return std::unexpected(document.error());
    }
    ParsedResult parsed;
    parsed.document = std::move(document.value());
    const json::Json object = parsed.document.root();
    if (!object.is_object()) {
        return std::unexpected(McpError::invalidResponse("result must be an object"));
    }
    auto resultType = requireResultType(object, parsed.result.typeName);
    if (!resultType) {
        return std::unexpected(resultType.error());
    }
    parsed.result.type = resultType.value();
    parsed.result.result = parsed.document.root();
    return parsed;
}

ToolCallResult ToolCallResult::text(std::string value)
{
    ToolCallResult result;
    std::string out;
    auto content = makeJsonWriter(out);
    // StreamWriter 失败粘滞：中间结果统一丢弃，由 finish() 统一检查
    (void)content.start_object();
    (void)content.key("type");
    (void)content.string("text");
    (void)content.key("text");
    (void)content.string(value);
    (void)content.end_object();
    if (content.finish()) {
        result.content.push_back(std::move(out));
    }
    return result;
}

std::string ToolCallResult::toJson() const
{
    std::string out;
    auto writer = makeJsonWriter(out);
    // StreamWriter 失败粘滞：中间结果统一丢弃，由 finish() 统一检查
    (void)writer.start_object();
    (void)writer.key("resultType");
    (void)writer.string("complete");
    writeImplementationMeta(writer, serverInfo, kServerInfoKey);
    (void)writer.key("content");
    (void)writer.start_array();
    for (const auto& item : content) {
        (void)writer.raw(item);
    }
    (void)writer.end_array();
    if (structuredContent) {
        (void)writer.key("structuredContent");
        (void)writer.raw(*structuredContent);
    }
    if (isError) {
        (void)writer.key("isError");
        (void)writer.boolean(true);
    }
    (void)writer.end_object();
    if (!writer.finish()) {
        return std::string{};
    }
    return std::move(out);
}

ReadResourceResult ReadResourceResult::text(std::string uri,
                                             std::string value,
                                             std::optional<std::string> mimeType)
{
    ReadResourceResult result;
    std::string out;
    auto content = makeJsonWriter(out);
    // StreamWriter 失败粘滞：中间结果统一丢弃，由 finish() 统一检查
    (void)content.start_object();
    (void)content.key("uri");
    (void)content.string(uri);
    if (mimeType) {
        (void)content.key("mimeType");
        (void)content.string(*mimeType);
    }
    (void)content.key("text");
    (void)content.string(value);
    (void)content.end_object();
    if (content.finish()) {
        result.contents.push_back(std::move(out));
    }
    return result;
}

std::string ReadResourceResult::toJson() const
{
    std::string out;
    auto writer = makeJsonWriter(out);
    // StreamWriter 失败粘滞：中间结果统一丢弃，由 finish() 统一检查
    (void)writer.start_object();
    writeCacheFields(writer, ttlMs, cacheScope);
    writeImplementationMeta(writer, serverInfo, kServerInfoKey);
    (void)writer.key("contents");
    (void)writer.start_array();
    for (const auto& item : contents) {
        (void)writer.raw(item);
    }
    (void)writer.end_array();
    (void)writer.end_object();
    if (!writer.finish()) {
        return std::string{};
    }
    return std::move(out);
}

std::string GetPromptResult::toJson() const
{
    std::string out;
    auto writer = makeJsonWriter(out);
    // StreamWriter 失败粘滞：中间结果统一丢弃，由 finish() 统一检查
    (void)writer.start_object();
    (void)writer.key("resultType");
    (void)writer.string("complete");
    writeImplementationMeta(writer, serverInfo, kServerInfoKey);
    writeOptionalString(writer, "description", description);
    (void)writer.key("messages");
    (void)writer.start_array();
    for (const auto& item : messages) {
        (void)writer.raw(item);
    }
    (void)writer.end_array();
    (void)writer.end_object();
    if (!writer.finish()) {
        return std::string{};
    }
    return std::move(out);
}

std::expected<ParsedResponse, McpError> parseResponse(std::string_view body)
{
    auto document = JsonDocument::parse(body);
    if (!document) {
        return std::unexpected(document.error());
    }

    ParsedResponse parsed;
    parsed.document = std::move(document.value());
    const json::Json object = parsed.document.root();
    if (!object.is_object()) {
        return std::unexpected(McpError::invalidResponse("response must be an object"));
    }
    const auto jsonrpc = object.at("jsonrpc").as_string();
    if (!jsonrpc || *jsonrpc != JSONRPC_VERSION) {
        return std::unexpected(McpError::invalidResponse("missing or invalid jsonrpc"));
    }

    const json::Json idElement = object.at("id");
    const bool hasId = idElement.valid();
    if (hasId) {
        auto id = parseRequestId(idElement);
        if (!id) {
            return std::unexpected(id.error());
        }
        parsed.response.id = std::move(id.value());
    }

    const json::Json resultElement = object.at("result");
    const json::Json errorElement = object.at("error");
    parsed.response.hasResult = resultElement.valid();
    parsed.response.hasError = errorElement.valid();
    if (parsed.response.hasResult == parsed.response.hasError) {
        return std::unexpected(McpError::invalidResponse(
            "response must contain exactly one of result or error"));
    }
    if (parsed.response.hasResult) {
        if (!hasId) {
            return std::unexpected(McpError::invalidResponse("result response missing id"));
        }
        if (!resultElement.is_object()) {
            return std::unexpected(McpError::invalidResponse("result must be an object"));
        }
        std::string unusedTypeName;
        auto resultType = requireResultType(resultElement, unusedTypeName);
        if (!resultType) {
            return std::unexpected(resultType.error());
        }
        parsed.response.result = resultElement;
        return parsed;
    }

    if (!errorElement.is_object()) {
        return std::unexpected(McpError::invalidResponse("error must be an object"));
    }
    const auto code = errorElement.at("code").as_int64();
    const auto message = errorElement.at("message").as_string();
    if (!code || !message) {
        return std::unexpected(McpError::invalidResponse("invalid error object"));
    }
    parsed.response.error = errorElement;
    return parsed;
}

std::string SubscriptionFilter::toJson() const
{
    std::string out;
    auto writer = makeJsonWriter(out);
    // StreamWriter 失败粘滞：中间结果统一丢弃，由 finish() 统一检查
    (void)writer.start_object();
    if (toolsListChanged) {
        (void)writer.key("toolsListChanged");
        (void)writer.boolean(true);
    }
    if (promptsListChanged) {
        (void)writer.key("promptsListChanged");
        (void)writer.boolean(true);
    }
    if (resourcesListChanged) {
        (void)writer.key("resourcesListChanged");
        (void)writer.boolean(true);
    }
    if (!resourceSubscriptions.empty()) {
        (void)writer.key("resourceSubscriptions");
        (void)writer.start_array();
        for (const auto& uri : resourceSubscriptions) {
            (void)writer.string(uri);
        }
        (void)writer.end_array();
    }
    (void)writer.end_object();
    if (!writer.finish()) {
        return std::string{};
    }
    return std::move(out);
}

std::expected<SubscriptionFilter, McpError> SubscriptionFilter::fromJson(
    const json::Json& element)
{
    auto objectResult = requireObject(element, "notifications");
    if (!objectResult) {
        return std::unexpected(objectResult.error());
    }
    const json::Json object = objectResult.value();
    SubscriptionFilter filter;
    auto readFlag = [&object](const char* key, bool& destination)
        -> std::expected<void, McpError> {
        const json::Json value = object.at(key);
        if (!value.valid()) {
            return {};
        }
        if (!value.is_bool()) {
            return std::unexpected(McpError::invalidParams(
                std::string(key) + " must be a boolean"));
        }
        destination = *value.as_bool();
        return {};
    };
    auto tools = readFlag("toolsListChanged", filter.toolsListChanged);
    auto prompts = readFlag("promptsListChanged", filter.promptsListChanged);
    auto resources = readFlag("resourcesListChanged", filter.resourcesListChanged);
    if (!tools) return std::unexpected(tools.error());
    if (!prompts) return std::unexpected(prompts.error());
    if (!resources) return std::unexpected(resources.error());

    const json::Json subscriptions = object.at("resourceSubscriptions");
    if (subscriptions.valid()) {
        if (!subscriptions.is_array()) {
            return std::unexpected(McpError::invalidParams(
                "resourceSubscriptions must be an array"));
        }
        for (size_t i = 0; i < subscriptions.size(); ++i) {
            const json::Json item = subscriptions.at(i);
            auto uri = item.as_string();
            if (!uri) {
                return std::unexpected(McpError::invalidParams(
                    "resourceSubscriptions must contain strings"));
            }
            filter.resourceSubscriptions.push_back(std::string(*uri));
        }
    }
    return filter;
}

std::string makeResultResponse(const RequestId& id, std::string_view resultJson)
{
    std::string out;
    auto writer = makeJsonWriter(out);
    // StreamWriter 失败粘滞：中间结果统一丢弃，由 finish() 统一检查
    (void)writer.start_object();
    (void)writer.key("jsonrpc");
    (void)writer.string(JSONRPC_VERSION);
    (void)writer.key("id");
    writeRequestId(writer, id);
    (void)writer.key("result");
    (void)writer.raw(std::string(resultJson));
    (void)writer.end_object();
    if (!writer.finish()) {
        return std::string{};
    }
    return std::move(out);
}

std::string makeErrorResponse(const std::optional<RequestId>& id,
                             int code,
                             std::string_view message,
                             std::optional<std::string_view> details)
{
    std::string out;
    auto writer = makeJsonWriter(out);
    // StreamWriter 失败粘滞：中间结果统一丢弃，由 finish() 统一检查
    (void)writer.start_object();
    (void)writer.key("jsonrpc");
    (void)writer.string(JSONRPC_VERSION);
    if (id) {
        (void)writer.key("id");
        writeRequestId(writer, *id);
    }
    (void)writer.key("error");
    (void)writer.start_object();
    (void)writer.key("code");
    (void)writer.number(static_cast<int64_t>(code));
    (void)writer.key("message");
    (void)writer.string(std::string(message));
    if (details) {
        (void)writer.key("data");
        (void)writer.string(std::string(*details));
    }
    (void)writer.end_object();
    (void)writer.end_object();
    if (!writer.finish()) {
        return std::string{};
    }
    return std::move(out);
}

std::string makeUnsupportedProtocolVersionResponse(
    const RequestId& id,
    std::string_view requested,
    const std::vector<std::string>& supported)
{
    std::string out;
    auto writer = makeJsonWriter(out);
    // StreamWriter 失败粘滞：中间结果统一丢弃，由 finish() 统一检查
    (void)writer.start_object();
    (void)writer.key("jsonrpc");
    (void)writer.string(JSONRPC_VERSION);
    (void)writer.key("id");
    writeRequestId(writer, id);
    (void)writer.key("error");
    (void)writer.start_object();
    (void)writer.key("code");
    (void)writer.number(static_cast<int64_t>(ErrorCodes::UNSUPPORTED_PROTOCOL_VERSION));
    (void)writer.key("message");
    (void)writer.string("Unsupported protocol version");
    (void)writer.key("data");
    (void)writer.start_object();
    (void)writer.key("supported");
    (void)writer.start_array();
    for (const auto& version : supported) {
        (void)writer.string(version);
    }
    (void)writer.end_array();
    (void)writer.key("requested");
    (void)writer.string(std::string(requested));
    (void)writer.end_object();
    (void)writer.end_object();
    (void)writer.end_object();
    if (!writer.finish()) {
        return std::string{};
    }
    return std::move(out);
}

std::string makeSubscriptionAcknowledgedNotification(
    const RequestId& id,
    const SubscriptionFilter& accepted)
{
    std::string out;
    auto writer = makeJsonWriter(out);
    // StreamWriter 失败粘滞：中间结果统一丢弃，由 finish() 统一检查
    (void)writer.start_object();
    (void)writer.key("jsonrpc");
    (void)writer.string(JSONRPC_VERSION);
    (void)writer.key("method");
    (void)writer.string(NotificationMethods::SUBSCRIPTIONS_ACKNOWLEDGED);
    (void)writer.key("params");
    (void)writer.start_object();
    (void)writer.key("_meta");
    (void)writer.start_object();
    (void)writer.key("io.modelcontextprotocol/subscriptionId");
    writeRequestId(writer, id);
    (void)writer.end_object();
    (void)writer.key("notifications");
    (void)writer.raw(accepted.toJson());
    (void)writer.end_object();
    (void)writer.end_object();
    if (!writer.finish()) {
        return std::string{};
    }
    return std::move(out);
}

std::string makeSubscriptionNotification(std::string_view method,
                                        const RequestId& id,
                                        std::optional<std::string_view> uri)
{
    std::string out;
    auto writer = makeJsonWriter(out);
    // StreamWriter 失败粘滞：中间结果统一丢弃，由 finish() 统一检查
    (void)writer.start_object();
    (void)writer.key("jsonrpc");
    (void)writer.string(JSONRPC_VERSION);
    (void)writer.key("method");
    (void)writer.string(std::string(method));
    (void)writer.key("params");
    (void)writer.start_object();
    (void)writer.key("_meta");
    (void)writer.start_object();
    (void)writer.key("io.modelcontextprotocol/subscriptionId");
    writeRequestId(writer, id);
    (void)writer.end_object();
    if (uri) {
        (void)writer.key("uri");
        (void)writer.string(std::string(*uri));
    }
    (void)writer.end_object();
    (void)writer.end_object();
    if (!writer.finish()) {
        return std::string{};
    }
    return std::move(out);
}

std::string makeSubscriptionCompleteResponse(const RequestId& id)
{
    std::string resultJson;
    auto result = makeJsonWriter(resultJson);
    // StreamWriter 失败粘滞：中间结果统一丢弃，由 finish() 统一检查
    (void)result.start_object();
    (void)result.key("resultType");
    (void)result.string("complete");
    (void)result.key("_meta");
    (void)result.start_object();
    (void)result.key("io.modelcontextprotocol/subscriptionId");
    writeRequestId(result, id);
    (void)result.end_object();
    (void)result.end_object();
    if (!result.finish()) {
        return std::string{};
    }
    return makeResultResponse(id, resultJson);
}

std::string encodeSseEvent(std::string_view message)
{
    std::string event;
    event.reserve(message.size() + 8);
    std::size_t offset = 0;
    while (offset <= message.size()) {
        const std::size_t newline = message.find('\n', offset);
        event += "data: ";
        if (newline == std::string_view::npos) {
            event.append(message.substr(offset));
            event += "\n\n";
            break;
        }
        event.append(message.substr(offset, newline - offset));
        event.push_back('\n');
        offset = newline + 1;
    }
    return event;
}

std::expected<std::optional<std::string>, McpError> parseSseEvent(
    std::string_view event)
{
    std::string data;
    bool hasData = false;
    std::size_t offset = 0;
    while (offset < event.size()) {
        std::size_t newline = event.find('\n', offset);
        if (newline == std::string_view::npos) newline = event.size();
        std::string_view line = event.substr(offset, newline - offset);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        offset = newline == event.size() ? event.size() : newline + 1;
        if (line.empty() || line.front() == ':') continue;
        const std::size_t colon = line.find(':');
        const std::string_view field = line.substr(0, colon);
        std::string_view value = colon == std::string_view::npos
            ? std::string_view{} : line.substr(colon + 1);
        if (!value.empty() && value.front() == ' ') value.remove_prefix(1);
        if (field != "data") continue;
        if (hasData) data.push_back('\n');
        data.append(value);
        hasData = true;
    }
    if (!hasData) return std::optional<std::string>{};
    if (!JsonDocument::parse(data)) {
        return std::unexpected(McpError::invalidResponse(
            "SSE data is not a JSON message"));
    }
    return std::optional<std::string>{std::move(data)};
}

} // namespace galay::mcp::v2
