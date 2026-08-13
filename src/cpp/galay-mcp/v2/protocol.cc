#include <galay/cpp/galay-mcp/v2/protocol.h>

#include <utility>

namespace galay::mcp::v2 {

namespace {

constexpr const char* kProtocolVersionKey = "io.modelcontextprotocol/protocolVersion";
constexpr const char* kClientCapabilitiesKey = "io.modelcontextprotocol/clientCapabilities";
constexpr const char* kClientInfoKey = "io.modelcontextprotocol/clientInfo";
constexpr const char* kServerInfoKey = "io.modelcontextprotocol/serverInfo";
constexpr const char* kLogLevelKey = "io.modelcontextprotocol/logLevel";

void writeRequestId(JsonWriter& writer, const RequestId& id)
{
    if (const auto* number = std::get_if<int64_t>(&id)) {
        writer.number(*number);
    } else {
        writer.string(std::get<std::string>(id));
    }
}

std::expected<RequestId, McpError> parseRequestId(const JsonElement& element)
{
    if (element.is_int64()) {
        return RequestId{element.get_int64().value()};
    }
    if (element.is_string()) {
        return RequestId{std::string(element.get_string().value())};
    }
    return std::unexpected(McpError::invalidRequest("id must be a string or integer"));
}

std::expected<JsonObject, McpError> requireObject(const JsonElement& element,
                                                   std::string_view context)
{
    JsonObject object;
    if (!JsonHelper::getObject(element, object)) {
        return std::unexpected(McpError::invalidParams(
            std::string(context) + " must be an object"));
    }
    return object;
}

std::expected<std::string, McpError> requireString(const JsonObject& object,
                                                    const char* key)
{
    std::string value;
    if (!JsonHelper::getString(object, key, value)) {
        return std::unexpected(McpError::invalidParams(
            std::string("missing or invalid ") + key));
    }
    return value;
}

void writeImplementationMeta(JsonWriter& writer,
                             const std::optional<Implementation>& implementation,
                             const char* key)
{
    if (!implementation) {
        return;
    }
    writer.key("_meta");
    writer.startObject();
    writer.key(key);
    writer.raw(implementation->toJson());
    writer.endObject();
}

void writeCacheFields(JsonWriter& writer, uint64_t ttlMs, CacheScope scope)
{
    writer.key("resultType");
    writer.string("complete");
    writer.key("ttlMs");
    writer.number(ttlMs);
    writer.key("cacheScope");
    writer.string(scope == CacheScope::Public ? "public" : "private");
}

void writeOptionalString(JsonWriter& writer,
                         const char* key,
                         const std::optional<std::string>& value)
{
    if (!value) {
        return;
    }
    writer.key(key);
    writer.string(*value);
}

void writeOptionalRaw(JsonWriter& writer,
                      const char* key,
                      const std::optional<JsonString>& value)
{
    if (!value) {
        return;
    }
    writer.key(key);
    writer.raw(*value);
}

std::expected<JsonString, McpError> rawJson(const JsonElement& element,
                                             std::string_view context)
{
    JsonString raw;
    if (!JsonHelper::getRawJson(element, raw)) {
        return std::unexpected(McpError::invalidParams(
            std::string("invalid ") + std::string(context)));
    }
    return raw;
}

std::expected<JsonString, McpError> requireRawObject(const JsonObject& object,
                                                      const char* key)
{
    JsonElement element;
    JsonObject nested;
    if (!JsonHelper::getElement(object, key, element) ||
        !JsonHelper::getObject(element, nested)) {
        return std::unexpected(McpError::invalidParams(
            std::string("missing or invalid ") + key));
    }
    return rawJson(element, key);
}

std::expected<std::optional<JsonString>, McpError> optionalRawObject(
    const JsonObject& object,
    const char* key)
{
    JsonElement element;
    if (!JsonHelper::getElement(object, key, element)) {
        return std::optional<JsonString>{};
    }
    JsonObject nested;
    if (!JsonHelper::getObject(element, nested)) {
        return std::unexpected(McpError::invalidParams(
            std::string("invalid ") + key));
    }
    auto raw = rawJson(element, key);
    if (!raw) {
        return std::unexpected(raw.error());
    }
    return std::optional<JsonString>{std::move(raw.value())};
}

std::expected<std::optional<JsonString>, McpError> optionalRawArray(
    const JsonObject& object,
    const char* key)
{
    JsonElement element;
    if (!JsonHelper::getElement(object, key, element)) {
        return std::optional<JsonString>{};
    }
    JsonArray nested;
    if (!JsonHelper::getArray(element, nested)) {
        return std::unexpected(McpError::invalidParams(
            std::string("invalid ") + key));
    }
    auto raw = rawJson(element, key);
    if (!raw) {
        return std::unexpected(raw.error());
    }
    return std::optional<JsonString>{std::move(raw.value())};
}

std::expected<uint64_t, McpError> requireUint64(const JsonObject& object,
                                                const char* key)
{
    JsonElement element;
    if (!JsonHelper::getElement(object, key, element)) {
        return std::unexpected(McpError::invalidParams(
            std::string("missing or invalid ") + key));
    }
    auto unsignedValue = element.get_uint64();
    if (!unsignedValue.error()) {
        return unsignedValue.value();
    }
    auto signedValue = element.get_int64();
    if (!signedValue.error() && signedValue.value() >= 0) {
        return static_cast<uint64_t>(signedValue.value());
    }
    return std::unexpected(McpError::invalidParams(
        std::string("missing or invalid ") + key));
}

std::expected<std::optional<uint64_t>, McpError> optionalUint64(
    const JsonObject& object,
    const char* key)
{
    JsonElement element;
    if (!JsonHelper::getElement(object, key, element)) {
        return std::optional<uint64_t>{};
    }
    auto unsignedValue = element.get_uint64();
    if (!unsignedValue.error()) {
        return std::optional<uint64_t>{unsignedValue.value()};
    }
    auto signedValue = element.get_int64();
    if (!signedValue.error() && signedValue.value() >= 0) {
        return std::optional<uint64_t>{static_cast<uint64_t>(signedValue.value())};
    }
    return std::unexpected(McpError::invalidParams(
        std::string("invalid ") + key));
}

std::expected<CacheScope, McpError> requireCacheScope(const JsonObject& object)
{
    std::string value;
    if (!JsonHelper::getString(object, "cacheScope", value)) {
        return std::unexpected(McpError::invalidParams("missing or invalid cacheScope"));
    }
    if (value == "public") {
        return CacheScope::Public;
    }
    if (value == "private") {
        return CacheScope::Private;
    }
    return std::unexpected(McpError::invalidParams("invalid cacheScope"));
}

std::expected<ResultType, McpError> requireResultType(const JsonObject& object,
                                                      std::string& typeName)
{
    if (!JsonHelper::getString(object, "resultType", typeName)) {
        return std::unexpected(McpError::invalidResponse("missing resultType"));
    }
    if (typeName == "complete") {
        return ResultType::Complete;
    }
    if (typeName == "input_required") {
        return ResultType::InputRequired;
    }
    return ResultType::Extension;
}

std::expected<std::optional<Implementation>, McpError> parseServerInfo(
    const JsonObject& object)
{
    JsonElement metaElement;
    if (!JsonHelper::getElement(object, "_meta", metaElement)) {
        return std::optional<Implementation>{};
    }
    JsonObject metaObject;
    if (!JsonHelper::getObject(metaElement, metaObject)) {
        return std::unexpected(McpError::invalidParams("invalid _meta"));
    }
    JsonElement serverInfoElement;
    if (!JsonHelper::getElement(metaObject, kServerInfoKey, serverInfoElement)) {
        return std::optional<Implementation>{};
    }
    auto serverInfo = Implementation::fromJson(serverInfoElement);
    if (!serverInfo) {
        return std::unexpected(serverInfo.error());
    }
    return std::optional<Implementation>{std::move(serverInfo.value())};
}

std::expected<void, McpError> parseCacheFields(const JsonObject& object,
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

std::expected<void, McpError> parseFeatureCapability(const JsonObject& object,
                                                     const char* key,
                                                     bool& present,
                                                     bool* listChanged,
                                                     bool* subscribe)
{
    JsonElement element;
    if (!JsonHelper::getElement(object, key, element)) {
        return {};
    }
    JsonObject feature;
    if (!JsonHelper::getObject(element, feature)) {
        return std::unexpected(McpError::invalidParams(
            std::string("invalid capability ") + key));
    }
    present = true;
    if (listChanged != nullptr) {
        bool value = false;
        if (JsonHelper::getBool(feature, "listChanged", value)) {
            *listChanged = value;
        }
    }
    if (subscribe != nullptr) {
        bool value = false;
        if (JsonHelper::getBool(feature, "subscribe", value)) {
            *subscribe = value;
        }
    }
    return {};
}

std::expected<std::vector<std::string>, McpError> requireStringArray(
    const JsonObject& object,
    const char* key)
{
    JsonElement element;
    JsonArray array;
    if (!JsonHelper::getElement(object, key, element) ||
        !JsonHelper::getArray(element, array)) {
        return std::unexpected(McpError::invalidParams(
            std::string("missing or invalid ") + key));
    }
    std::vector<std::string> values;
    for (auto item : array) {
        std::string value;
        if (!JsonHelper::getStringValue(item, value)) {
            return std::unexpected(McpError::invalidParams(
                std::string("invalid ") + key));
        }
        values.push_back(std::move(value));
    }
    return values;
}

std::expected<std::vector<PromptArgument>, McpError> parsePromptArguments(
    const JsonObject& object)
{
    JsonElement element;
    if (!JsonHelper::getElement(object, "arguments", element)) {
        return std::vector<PromptArgument>{};
    }
    JsonArray array;
    if (!JsonHelper::getArray(element, array)) {
        return std::unexpected(McpError::invalidParams("invalid arguments"));
    }
    std::vector<PromptArgument> arguments;
    for (auto item : array) {
        auto argument = PromptArgument::fromJson(item);
        if (!argument) {
            return std::unexpected(argument.error());
        }
        arguments.push_back(std::move(argument.value()));
    }
    return arguments;
}

} // namespace

JsonString Implementation::toJson() const
{
    JsonWriter writer;
    writer.startObject();
    writer.key("name");
    writer.string(name);
    writer.key("version");
    writer.string(version);
    if (title) {
        writer.key("title");
        writer.string(*title);
    }
    if (description) {
        writer.key("description");
        writer.string(*description);
    }
    if (websiteUrl) {
        writer.key("websiteUrl");
        writer.string(*websiteUrl);
    }
    if (!icons.empty()) {
        writer.key("icons");
        writer.raw(icons);
    }
    writer.endObject();
    return writer.takeString();
}

std::expected<Implementation, McpError> Implementation::fromJson(const JsonElement& element)
{
    auto objectResult = requireObject(element, "implementation");
    if (!objectResult) {
        return std::unexpected(objectResult.error());
    }
    const JsonObject object = objectResult.value();
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
    std::string optional;
    if (JsonHelper::getString(object, "title", optional)) {
        implementation.title = optional;
    }
    if (JsonHelper::getString(object, "description", optional)) {
        implementation.description = optional;
    }
    if (JsonHelper::getString(object, "websiteUrl", optional)) {
        implementation.websiteUrl = optional;
    }
    JsonElement iconsElement;
    if (JsonHelper::getElement(object, "icons", iconsElement)) {
        JsonString raw;
        if (!JsonHelper::getRawJson(iconsElement, raw)) {
            return std::unexpected(McpError::invalidParams("invalid icons"));
        }
        implementation.icons = std::move(raw);
    }
    return implementation;
}

JsonString RequestMeta::toJson() const
{
    JsonWriter writer;
    writer.startObject();
    if (progressToken) {
        writer.key("progressToken");
        writeRequestId(writer, *progressToken);
    }
    writer.key(kProtocolVersionKey);
    writer.string(protocolVersion);
    if (clientInfo) {
        writer.key(kClientInfoKey);
        writer.raw(clientInfo->toJson());
    }
    writer.key(kClientCapabilitiesKey);
    writer.raw(clientCapabilities.empty() ? "{}" : clientCapabilities);
    if (logLevel) {
        writer.key(kLogLevelKey);
        writer.string(*logLevel);
    }
    writer.endObject();
    return writer.takeString();
}

std::expected<RequestMeta, McpError> RequestMeta::fromJson(const JsonElement& element)
{
    auto objectResult = requireObject(element, "_meta");
    if (!objectResult) {
        return std::unexpected(objectResult.error());
    }
    const JsonObject object = objectResult.value();

    RequestMeta meta;
    auto protocolVersion = requireString(object, kProtocolVersionKey);
    if (!protocolVersion) {
        return std::unexpected(protocolVersion.error());
    }
    meta.protocolVersion = std::move(protocolVersion.value());

    JsonElement capabilitiesElement;
    JsonObject capabilitiesObject;
    if (!JsonHelper::getElement(object, kClientCapabilitiesKey, capabilitiesElement) ||
        !JsonHelper::getObject(capabilitiesElement, capabilitiesObject) ||
        !JsonHelper::getRawJson(capabilitiesElement, meta.clientCapabilities)) {
        return std::unexpected(McpError::invalidParams(
            "missing or invalid io.modelcontextprotocol/clientCapabilities"));
    }

    JsonElement clientInfoElement;
    if (JsonHelper::getElement(object, kClientInfoKey, clientInfoElement)) {
        auto clientInfo = Implementation::fromJson(clientInfoElement);
        if (!clientInfo) {
            return std::unexpected(clientInfo.error());
        }
        meta.clientInfo = std::move(clientInfo.value());
    }

    std::string logLevel;
    if (JsonHelper::getString(object, kLogLevelKey, logLevel)) {
        meta.logLevel = std::move(logLevel);
    }

    JsonElement progressToken;
    if (JsonHelper::getElement(object, "progressToken", progressToken)) {
        auto parsed = parseRequestId(progressToken);
        if (!parsed) {
            return std::unexpected(McpError::invalidParams("invalid progressToken"));
        }
        meta.progressToken = std::move(parsed.value());
    }
    return meta;
}

JsonString ServerCapabilities::toJson() const
{
    JsonWriter writer;
    writer.startObject();
    if (!extensions.empty()) {
        writer.key("extensions");
        writer.raw(extensions);
    }
    if (tools) {
        writer.key("tools");
        writer.startObject();
        if (toolsListChanged) {
            writer.key("listChanged");
            writer.boolean(true);
        }
        writer.endObject();
    }
    if (resources) {
        writer.key("resources");
        writer.startObject();
        if (resourceSubscriptions) {
            writer.key("subscribe");
            writer.boolean(true);
        }
        if (resourcesListChanged) {
            writer.key("listChanged");
            writer.boolean(true);
        }
        writer.endObject();
    }
    if (prompts) {
        writer.key("prompts");
        writer.startObject();
        if (promptsListChanged) {
            writer.key("listChanged");
            writer.boolean(true);
        }
        writer.endObject();
    }
    if (completions) {
        writer.key("completions");
        writer.startObject();
        writer.endObject();
    }
    if (logging) {
        writer.key("logging");
        writer.startObject();
        writer.endObject();
    }
    writer.endObject();
    return writer.takeString();
}

std::expected<ServerCapabilities, McpError> ServerCapabilities::fromJson(
    const JsonElement& element)
{
    auto objectResult = requireObject(element, "capabilities");
    if (!objectResult) {
        return std::unexpected(objectResult.error());
    }
    const JsonObject object = objectResult.value();
    ServerCapabilities capabilities;

    auto extensions = optionalRawObject(object, "extensions");
    if (!extensions) {
        return std::unexpected(extensions.error());
    }
    capabilities.extensions = std::move(extensions.value().value_or(JsonString{}));

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

JsonString Tool::toJson() const
{
    JsonWriter writer;
    writer.startObject();
    writer.key("name");
    writer.string(name);
    writeOptionalString(writer, "title", title);
    writeOptionalString(writer, "description", description);
    writer.key("inputSchema");
    writer.raw(inputSchema.empty() ? "{\"type\":\"object\"}" : inputSchema);
    writeOptionalRaw(writer, "outputSchema", outputSchema);
    writeOptionalRaw(writer, "annotations", annotations);
    writeOptionalRaw(writer, "icons", icons);
    writeOptionalRaw(writer, "_meta", meta);
    writer.endObject();
    return writer.takeString();
}

std::expected<Tool, McpError> Tool::fromJson(const JsonElement& element)
{
    auto objectResult = requireObject(element, "tool");
    if (!objectResult) {
        return std::unexpected(objectResult.error());
    }
    const JsonObject object = objectResult.value();
    auto name = requireString(object, "name");
    if (!name) {
        return std::unexpected(name.error());
    }
    auto inputSchema = requireRawObject(object, "inputSchema");
    if (!inputSchema) {
        return std::unexpected(inputSchema.error());
    }
    JsonElement inputElement;
    JsonObject inputObject;
    std::string type;
    if (!JsonHelper::getElement(object, "inputSchema", inputElement) ||
        !JsonHelper::getObject(inputElement, inputObject) ||
        !JsonHelper::getString(inputObject, "type", type) ||
        type != "object") {
        return std::unexpected(McpError::invalidParams(
            "tool inputSchema root type must be object"));
    }

    Tool tool;
    tool.name = std::move(name.value());
    tool.inputSchema = std::move(inputSchema.value());
    std::string optional;
    if (JsonHelper::getString(object, "title", optional)) {
        tool.title = optional;
    }
    if (JsonHelper::getString(object, "description", optional)) {
        tool.description = optional;
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

JsonString Resource::toJson() const
{
    JsonWriter writer;
    writer.startObject();
    writer.key("uri");
    writer.string(uri);
    writer.key("name");
    writer.string(name);
    writeOptionalString(writer, "title", title);
    writeOptionalString(writer, "description", description);
    writeOptionalString(writer, "mimeType", mimeType);
    if (size) {
        writer.key("size");
        writer.number(*size);
    }
    writeOptionalRaw(writer, "annotations", annotations);
    writeOptionalRaw(writer, "icons", icons);
    writeOptionalRaw(writer, "_meta", meta);
    writer.endObject();
    return writer.takeString();
}

std::expected<Resource, McpError> Resource::fromJson(const JsonElement& element)
{
    auto objectResult = requireObject(element, "resource");
    if (!objectResult) {
        return std::unexpected(objectResult.error());
    }
    const JsonObject object = objectResult.value();
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
    std::string optional;
    if (JsonHelper::getString(object, "title", optional)) {
        resource.title = optional;
    }
    if (JsonHelper::getString(object, "description", optional)) {
        resource.description = optional;
    }
    if (JsonHelper::getString(object, "mimeType", optional)) {
        resource.mimeType = optional;
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

JsonString PromptArgument::toJson() const
{
    JsonWriter writer;
    writer.startObject();
    writer.key("name");
    writer.string(name);
    writeOptionalString(writer, "title", title);
    writeOptionalString(writer, "description", description);
    if (required) {
        writer.key("required");
        writer.boolean(true);
    }
    writer.endObject();
    return writer.takeString();
}

std::expected<PromptArgument, McpError> PromptArgument::fromJson(
    const JsonElement& element)
{
    auto objectResult = requireObject(element, "prompt argument");
    if (!objectResult) {
        return std::unexpected(objectResult.error());
    }
    const JsonObject object = objectResult.value();
    auto name = requireString(object, "name");
    if (!name) {
        return std::unexpected(name.error());
    }
    PromptArgument argument;
    argument.name = std::move(name.value());
    std::string optional;
    if (JsonHelper::getString(object, "title", optional)) {
        argument.title = optional;
    }
    if (JsonHelper::getString(object, "description", optional)) {
        argument.description = optional;
    }
    bool required = false;
    if (JsonHelper::getBool(object, "required", required)) {
        argument.required = required;
    }
    return argument;
}

JsonString Prompt::toJson() const
{
    JsonWriter writer;
    writer.startObject();
    writer.key("name");
    writer.string(name);
    writeOptionalString(writer, "title", title);
    writeOptionalString(writer, "description", description);
    if (!arguments.empty()) {
        writer.key("arguments");
        writer.startArray();
        for (const auto& argument : arguments) {
            writer.raw(argument.toJson());
        }
        writer.endArray();
    }
    writeOptionalRaw(writer, "icons", icons);
    writeOptionalRaw(writer, "_meta", meta);
    writer.endObject();
    return writer.takeString();
}

std::expected<Prompt, McpError> Prompt::fromJson(const JsonElement& element)
{
    auto objectResult = requireObject(element, "prompt");
    if (!objectResult) {
        return std::unexpected(objectResult.error());
    }
    const JsonObject object = objectResult.value();
    auto name = requireString(object, "name");
    if (!name) {
        return std::unexpected(name.error());
    }
    Prompt prompt;
    prompt.name = std::move(name.value());
    std::string optional;
    if (JsonHelper::getString(object, "title", optional)) {
        prompt.title = optional;
    }
    if (JsonHelper::getString(object, "description", optional)) {
        prompt.description = optional;
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

JsonString DiscoverResult::toJson() const
{
    JsonWriter writer;
    writer.startObject();
    writeCacheFields(writer, ttlMs, cacheScope);
    writeImplementationMeta(writer, serverInfo, kServerInfoKey);
    writer.key("supportedVersions");
    writer.startArray();
    for (const auto& version : supportedVersions) {
        writer.string(version);
    }
    writer.endArray();
    writer.key("capabilities");
    writer.raw(capabilities.toJson());
    if (instructions) {
        writer.key("instructions");
        writer.string(*instructions);
    }
    writer.endObject();
    return writer.takeString();
}

std::expected<DiscoverResult, McpError> DiscoverResult::fromJson(
    const JsonElement& element)
{
    auto objectResult = requireObject(element, "discover result");
    if (!objectResult) {
        return std::unexpected(objectResult.error());
    }
    const JsonObject object = objectResult.value();
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
    JsonElement capabilitiesElement;
    if (!JsonHelper::getElement(object, "capabilities", capabilitiesElement)) {
        return std::unexpected(McpError::invalidParams("missing capabilities"));
    }
    auto capabilities = ServerCapabilities::fromJson(capabilitiesElement);
    if (!capabilities) {
        return std::unexpected(capabilities.error());
    }
    result.capabilities = std::move(capabilities.value());
    std::string instructions;
    if (JsonHelper::getString(object, "instructions", instructions)) {
        result.instructions = std::move(instructions);
    }
    auto serverInfo = parseServerInfo(object);
    if (!serverInfo) {
        return std::unexpected(serverInfo.error());
    }
    result.serverInfo = std::move(serverInfo.value());
    return result;
}

JsonString ListResult::toJson() const
{
    JsonWriter writer;
    writer.startObject();
    writeCacheFields(writer, ttlMs, cacheScope);
    writeImplementationMeta(writer, serverInfo, kServerInfoKey);
    writer.key(field);
    writer.startArray();
    for (const auto& item : items) {
        writer.raw(item);
    }
    writer.endArray();
    if (nextCursor) {
        writer.key("nextCursor");
        writer.string(*nextCursor);
    }
    writer.endObject();
    return writer.takeString();
}

JsonString JsonRpcRequest::toJson() const
{
    JsonWriter writer;
    writer.startObject();
    writer.key("jsonrpc");
    writer.string(JSONRPC_VERSION);
    writer.key("id");
    writeRequestId(writer, id);
    writer.key("method");
    writer.string(method);
    if (params) {
        writer.key("params");
        writer.raw(*params);
    }
    writer.endObject();
    return writer.takeString();
}

JsonString makeRequestParams(const RequestMeta& meta)
{
    JsonWriter writer;
    writer.startObject();
    writer.key("_meta");
    writer.raw(meta.toJson());
    writer.endObject();
    return writer.takeString();
}

std::expected<JsonString, McpError> makeRequestParams(const RequestMeta& meta,
                                                      std::string_view fieldsJson)
{
    auto document = JsonDocument::parse(fieldsJson);
    if (!document) {
        return std::unexpected(document.error());
    }
    JsonObject fields;
    if (!JsonHelper::getObject(document->root(), fields)) {
        return std::unexpected(McpError::invalidParams("request fields must be an object"));
    }

    JsonWriter writer;
    writer.startObject();
    for (auto field : fields) {
        const std::string key(field.key);
        if (key == "_meta") {
            return std::unexpected(McpError::invalidParams(
                "request fields must not contain _meta"));
        }
        auto raw = rawJson(field.value, key);
        if (!raw) {
            return std::unexpected(raw.error());
        }
        writer.key(key);
        writer.raw(raw.value());
    }
    writer.key("_meta");
    writer.raw(meta.toJson());
    writer.endObject();
    return writer.takeString();
}

std::expected<ParsedRequest, McpError> parseRequest(std::string_view body)
{
    auto document = JsonDocument::parse(body);
    if (!document) {
        return std::unexpected(document.error());
    }

    ParsedRequest parsed;
    parsed.document = std::move(document.value());
    JsonObject object;
    if (!JsonHelper::getObject(parsed.document.root(), object)) {
        return std::unexpected(McpError::invalidRequest("request must be an object"));
    }
    std::string jsonrpc;
    if (!JsonHelper::getString(object, "jsonrpc", jsonrpc) || jsonrpc != JSONRPC_VERSION) {
        return std::unexpected(McpError::invalidRequest("missing or invalid jsonrpc"));
    }
    auto method = requireString(object, "method");
    if (!method) {
        return std::unexpected(McpError::invalidRequest(method.error().details()));
    }
    parsed.request.method = std::move(method.value());

    JsonElement idElement;
    if (!JsonHelper::getElement(object, "id", idElement)) {
        return std::unexpected(McpError::invalidRequest("missing id"));
    }
    auto id = parseRequestId(idElement);
    if (!id) {
        return std::unexpected(id.error());
    }
    parsed.request.id = std::move(id.value());

    if (!JsonHelper::getElement(object, "params", parsed.request.params)) {
        return std::unexpected(McpError::invalidParams("missing params"));
    }
    JsonObject paramsObject;
    if (!JsonHelper::getObject(parsed.request.params, paramsObject)) {
        return std::unexpected(McpError::invalidParams("params must be an object"));
    }
    JsonElement metaElement;
    if (!JsonHelper::getElement(paramsObject, "_meta", metaElement)) {
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
    JsonObject object;
    if (!JsonHelper::getObject(parsed.document.root(), object)) {
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
    JsonWriter content;
    content.startObject();
    content.key("type");
    content.string("text");
    content.key("text");
    content.string(value);
    content.endObject();
    result.content.push_back(content.takeString());
    return result;
}

JsonString ToolCallResult::toJson() const
{
    JsonWriter writer;
    writer.startObject();
    writer.key("resultType");
    writer.string("complete");
    writeImplementationMeta(writer, serverInfo, kServerInfoKey);
    writer.key("content");
    writer.startArray();
    for (const auto& item : content) {
        writer.raw(item);
    }
    writer.endArray();
    if (structuredContent) {
        writer.key("structuredContent");
        writer.raw(*structuredContent);
    }
    if (isError) {
        writer.key("isError");
        writer.boolean(true);
    }
    writer.endObject();
    return writer.takeString();
}

ReadResourceResult ReadResourceResult::text(std::string uri,
                                             std::string value,
                                             std::optional<std::string> mimeType)
{
    ReadResourceResult result;
    JsonWriter content;
    content.startObject();
    content.key("uri");
    content.string(uri);
    if (mimeType) {
        content.key("mimeType");
        content.string(*mimeType);
    }
    content.key("text");
    content.string(value);
    content.endObject();
    result.contents.push_back(content.takeString());
    return result;
}

JsonString ReadResourceResult::toJson() const
{
    JsonWriter writer;
    writer.startObject();
    writeCacheFields(writer, ttlMs, cacheScope);
    writeImplementationMeta(writer, serverInfo, kServerInfoKey);
    writer.key("contents");
    writer.startArray();
    for (const auto& item : contents) {
        writer.raw(item);
    }
    writer.endArray();
    writer.endObject();
    return writer.takeString();
}

JsonString GetPromptResult::toJson() const
{
    JsonWriter writer;
    writer.startObject();
    writer.key("resultType");
    writer.string("complete");
    writeImplementationMeta(writer, serverInfo, kServerInfoKey);
    writeOptionalString(writer, "description", description);
    writer.key("messages");
    writer.startArray();
    for (const auto& item : messages) {
        writer.raw(item);
    }
    writer.endArray();
    writer.endObject();
    return writer.takeString();
}

std::expected<ParsedResponse, McpError> parseResponse(std::string_view body)
{
    auto document = JsonDocument::parse(body);
    if (!document) {
        return std::unexpected(document.error());
    }

    ParsedResponse parsed;
    parsed.document = std::move(document.value());
    JsonObject object;
    if (!JsonHelper::getObject(parsed.document.root(), object)) {
        return std::unexpected(McpError::invalidResponse("response must be an object"));
    }
    std::string jsonrpc;
    if (!JsonHelper::getString(object, "jsonrpc", jsonrpc) || jsonrpc != JSONRPC_VERSION) {
        return std::unexpected(McpError::invalidResponse("missing or invalid jsonrpc"));
    }

    JsonElement idElement;
    const bool hasId = JsonHelper::getElement(object, "id", idElement);
    if (hasId) {
        auto id = parseRequestId(idElement);
        if (!id) {
            return std::unexpected(id.error());
        }
        parsed.response.id = std::move(id.value());
    }

    JsonElement resultElement;
    JsonElement errorElement;
    parsed.response.hasResult = JsonHelper::getElement(object, "result", resultElement);
    parsed.response.hasError = JsonHelper::getElement(object, "error", errorElement);
    if (parsed.response.hasResult == parsed.response.hasError) {
        return std::unexpected(McpError::invalidResponse(
            "response must contain exactly one of result or error"));
    }
    if (parsed.response.hasResult) {
        if (!hasId) {
            return std::unexpected(McpError::invalidResponse("result response missing id"));
        }
        JsonObject resultObject;
        if (!JsonHelper::getObject(resultElement, resultObject)) {
            return std::unexpected(McpError::invalidResponse("result must be an object"));
        }
        std::string unusedTypeName;
        auto resultType = requireResultType(resultObject, unusedTypeName);
        if (!resultType) {
            return std::unexpected(resultType.error());
        }
        parsed.response.result = resultElement;
        return parsed;
    }

    JsonObject errorObject;
    if (!JsonHelper::getObject(errorElement, errorObject)) {
        return std::unexpected(McpError::invalidResponse("error must be an object"));
    }
    int64_t code = 0;
    std::string message;
    if (!JsonHelper::getInt64(errorObject, "code", code) ||
        !JsonHelper::getString(errorObject, "message", message)) {
        return std::unexpected(McpError::invalidResponse("invalid error object"));
    }
    parsed.response.error = errorElement;
    return parsed;
}

JsonString SubscriptionFilter::toJson() const
{
    JsonWriter writer;
    writer.startObject();
    if (toolsListChanged) {
        writer.key("toolsListChanged");
        writer.boolean(true);
    }
    if (promptsListChanged) {
        writer.key("promptsListChanged");
        writer.boolean(true);
    }
    if (resourcesListChanged) {
        writer.key("resourcesListChanged");
        writer.boolean(true);
    }
    if (!resourceSubscriptions.empty()) {
        writer.key("resourceSubscriptions");
        writer.startArray();
        for (const auto& uri : resourceSubscriptions) {
            writer.string(uri);
        }
        writer.endArray();
    }
    writer.endObject();
    return writer.takeString();
}

std::expected<SubscriptionFilter, McpError> SubscriptionFilter::fromJson(
    const JsonElement& element)
{
    auto objectResult = requireObject(element, "notifications");
    if (!objectResult) {
        return std::unexpected(objectResult.error());
    }
    const JsonObject object = objectResult.value();
    SubscriptionFilter filter;
    auto readFlag = [&object](const char* key, bool& destination)
        -> std::expected<void, McpError> {
        JsonElement value;
        if (!JsonHelper::getElement(object, key, value)) {
            return {};
        }
        if (!value.is_bool()) {
            return std::unexpected(McpError::invalidParams(
                std::string(key) + " must be a boolean"));
        }
        destination = value.get_bool().value();
        return {};
    };
    auto tools = readFlag("toolsListChanged", filter.toolsListChanged);
    auto prompts = readFlag("promptsListChanged", filter.promptsListChanged);
    auto resources = readFlag("resourcesListChanged", filter.resourcesListChanged);
    if (!tools) return std::unexpected(tools.error());
    if (!prompts) return std::unexpected(prompts.error());
    if (!resources) return std::unexpected(resources.error());

    JsonElement subscriptionsElement;
    if (JsonHelper::getElement(object, "resourceSubscriptions", subscriptionsElement)) {
        JsonArray subscriptions;
        if (!JsonHelper::getArray(subscriptionsElement, subscriptions)) {
            return std::unexpected(McpError::invalidParams(
                "resourceSubscriptions must be an array"));
        }
        for (auto item : subscriptions) {
            std::string uri;
            if (!JsonHelper::getStringValue(item, uri)) {
                return std::unexpected(McpError::invalidParams(
                    "resourceSubscriptions must contain strings"));
            }
            filter.resourceSubscriptions.push_back(std::move(uri));
        }
    }
    return filter;
}

JsonString makeResultResponse(const RequestId& id, std::string_view resultJson)
{
    JsonWriter writer;
    writer.startObject();
    writer.key("jsonrpc");
    writer.string(JSONRPC_VERSION);
    writer.key("id");
    writeRequestId(writer, id);
    writer.key("result");
    writer.raw(std::string(resultJson));
    writer.endObject();
    return writer.takeString();
}

JsonString makeErrorResponse(const std::optional<RequestId>& id,
                             int code,
                             std::string_view message,
                             std::optional<std::string_view> dataJson)
{
    JsonWriter writer;
    writer.startObject();
    writer.key("jsonrpc");
    writer.string(JSONRPC_VERSION);
    if (id) {
        writer.key("id");
        writeRequestId(writer, *id);
    }
    writer.key("error");
    writer.startObject();
    writer.key("code");
    writer.number(static_cast<int64_t>(code));
    writer.key("message");
    writer.string(std::string(message));
    if (dataJson) {
        writer.key("data");
        writer.raw(std::string(*dataJson));
    }
    writer.endObject();
    writer.endObject();
    return writer.takeString();
}

JsonString makeUnsupportedProtocolVersionResponse(
    const RequestId& id,
    std::string_view requested,
    const std::vector<std::string>& supported)
{
    JsonWriter data;
    data.startObject();
    data.key("supported");
    data.startArray();
    for (const auto& version : supported) {
        data.string(version);
    }
    data.endArray();
    data.key("requested");
    data.string(std::string(requested));
    data.endObject();
    const auto dataJson = data.takeString();
    return makeErrorResponse(id,
                             ErrorCodes::UNSUPPORTED_PROTOCOL_VERSION,
                             "Unsupported protocol version",
                             dataJson);
}

JsonString makeSubscriptionAcknowledgedNotification(
    const RequestId& id,
    const SubscriptionFilter& accepted)
{
    JsonWriter writer;
    writer.startObject();
    writer.key("jsonrpc");
    writer.string(JSONRPC_VERSION);
    writer.key("method");
    writer.string(NotificationMethods::SUBSCRIPTIONS_ACKNOWLEDGED);
    writer.key("params");
    writer.startObject();
    writer.key("_meta");
    writer.startObject();
    writer.key("io.modelcontextprotocol/subscriptionId");
    writeRequestId(writer, id);
    writer.endObject();
    writer.key("notifications");
    writer.raw(accepted.toJson());
    writer.endObject();
    writer.endObject();
    return writer.takeString();
}

JsonString makeSubscriptionNotification(std::string_view method,
                                        const RequestId& id,
                                        std::optional<std::string_view> uri)
{
    JsonWriter writer;
    writer.startObject();
    writer.key("jsonrpc");
    writer.string(JSONRPC_VERSION);
    writer.key("method");
    writer.string(std::string(method));
    writer.key("params");
    writer.startObject();
    writer.key("_meta");
    writer.startObject();
    writer.key("io.modelcontextprotocol/subscriptionId");
    writeRequestId(writer, id);
    writer.endObject();
    if (uri) {
        writer.key("uri");
        writer.string(std::string(*uri));
    }
    writer.endObject();
    writer.endObject();
    return writer.takeString();
}

JsonString makeSubscriptionCompleteResponse(const RequestId& id)
{
    JsonWriter result;
    result.startObject();
    result.key("resultType");
    result.string("complete");
    result.key("_meta");
    result.startObject();
    result.key("io.modelcontextprotocol/subscriptionId");
    writeRequestId(result, id);
    result.endObject();
    result.endObject();
    return makeResultResponse(id, result.takeString());
}

JsonString encodeSseEvent(std::string_view message)
{
    JsonString event;
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

std::expected<std::optional<JsonString>, McpError> parseSseEvent(
    std::string_view event)
{
    JsonString data;
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
    if (!hasData) return std::optional<JsonString>{};
    if (!JsonDocument::parse(data)) {
        return std::unexpected(McpError::invalidResponse(
            "SSE data is not a JSON message"));
    }
    return std::optional<JsonString>{std::move(data)};
}

} // namespace galay::mcp::v2
