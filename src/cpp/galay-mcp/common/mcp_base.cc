#include <galay/cpp/galay-mcp/common/mcp_base.h>

namespace galay {
namespace mcp {

namespace {

std::expected<JsonObject, McpError> requireObject(const JsonElement& element, const char* context) {
    JsonObject obj;
    if (!JsonHelper::getObject(element, obj)) {
        return std::unexpected(McpError::invalidMessage(std::string("Expected object for ") + context));
    }
    return obj;
}

std::expected<std::string, McpError> requireString(const JsonObject& obj, const char* key) {
    std::string value;
    if (!JsonHelper::getString(obj, key, value)) {
        return std::unexpected(McpError::invalidMessage(std::string("Missing or invalid ") + key));
    }
    return value;
}

std::expected<int64_t, McpError> requireInt64(const JsonObject& obj, const char* key) {
    int64_t value = 0;
    if (!JsonHelper::getInt64(obj, key, value)) {
        return std::unexpected(McpError::invalidMessage(std::string("Missing or invalid ") + key));
    }
    return value;
}

void writeRawOrEmptyObject(JsonWriter& writer, const JsonString& raw) {
    if (raw.empty()) {
        writer.startObject();
        writer.endObject();
        return;
    }
    writer.raw(raw);
}

} // namespace

JsonString Content::toJson() const {
    JsonWriter writer;
    writer.startObject();
    switch (type) {
        case ContentType::Text:
            writer.key("type");
            writer.string("text");
            writer.key("text");
            writer.string(text);
            break;
        case ContentType::Image:
            writer.key("type");
            writer.string("image");
            writer.key("data");
            writer.string(data);
            writer.key("mimeType");
            writer.string(mimeType);
            break;
        case ContentType::Resource:
            writer.key("type");
            writer.string("resource");
            writer.key("uri");
            writer.string(uri);
            break;
    }
    writer.endObject();
    return writer.takeString();
}

std::expected<Content, McpError> Content::fromJson(const JsonElement& element) {
    auto objExp = requireObject(element, "content");
    if (!objExp) {
        return std::unexpected(objExp.error());
    }
    JsonObject obj = objExp.value();

    auto typeStrExp = requireString(obj, "type");
    if (!typeStrExp) {
        return std::unexpected(typeStrExp.error());
    }

    Content c;
    const std::string& typeStr = typeStrExp.value();
    if (typeStr == "text") {
        c.type = ContentType::Text;
        auto textExp = requireString(obj, "text");
        if (!textExp) {
            return std::unexpected(textExp.error());
        }
        c.text = textExp.value();
    } else if (typeStr == "image") {
        c.type = ContentType::Image;
        auto dataExp = requireString(obj, "data");
        if (!dataExp) {
            return std::unexpected(dataExp.error());
        }
        auto mimeExp = requireString(obj, "mimeType");
        if (!mimeExp) {
            return std::unexpected(mimeExp.error());
        }
        c.data = dataExp.value();
        c.mimeType = mimeExp.value();
    } else if (typeStr == "resource") {
        c.type = ContentType::Resource;
        auto uriExp = requireString(obj, "uri");
        if (!uriExp) {
            return std::unexpected(uriExp.error());
        }
        c.uri = uriExp.value();
    } else {
        return std::unexpected(McpError::invalidMessage("Unknown content type"));
    }

    return c;
}

JsonString Tool::toJson() const {
    JsonWriter writer;
    writer.startObject();
    writer.key("name");
    writer.string(name);
    writer.key("description");
    writer.string(description);
    writer.key("inputSchema");
    writeRawOrEmptyObject(writer, inputSchema);
    writer.endObject();
    return writer.takeString();
}

std::expected<Tool, McpError> Tool::fromJson(const JsonElement& element) {
    auto objExp = requireObject(element, "tool");
    if (!objExp) {
        return std::unexpected(objExp.error());
    }
    JsonObject obj = objExp.value();

    Tool t;
    auto nameExp = requireString(obj, "name");
    if (!nameExp) {
        return std::unexpected(nameExp.error());
    }
    auto descExp = requireString(obj, "description");
    if (!descExp) {
        return std::unexpected(descExp.error());
    }
    t.name = nameExp.value();
    t.description = descExp.value();

    JsonElement schemaElement;
    if (JsonHelper::getElement(obj, "inputSchema", schemaElement)) {
        std::string raw;
        if (JsonHelper::getRawJson(schemaElement, raw)) {
            t.inputSchema = std::move(raw);
        }
    }

    return t;
}

JsonString Resource::toJson() const {
    JsonWriter writer;
    writer.startObject();
    writer.key("uri");
    writer.string(uri);
    writer.key("name");
    writer.string(name);
    writer.key("description");
    writer.string(description);
    writer.key("mimeType");
    writer.string(mimeType);
    writer.endObject();
    return writer.takeString();
}

std::expected<Resource, McpError> Resource::fromJson(const JsonElement& element) {
    auto objExp = requireObject(element, "resource");
    if (!objExp) {
        return std::unexpected(objExp.error());
    }
    JsonObject obj = objExp.value();

    Resource r;
    auto uriExp = requireString(obj, "uri");
    if (!uriExp) {
        return std::unexpected(uriExp.error());
    }
    auto nameExp = requireString(obj, "name");
    if (!nameExp) {
        return std::unexpected(nameExp.error());
    }
    auto descExp = requireString(obj, "description");
    if (!descExp) {
        return std::unexpected(descExp.error());
    }
    auto mimeExp = requireString(obj, "mimeType");
    if (!mimeExp) {
        return std::unexpected(mimeExp.error());
    }

    r.uri = uriExp.value();
    r.name = nameExp.value();
    r.description = descExp.value();
    r.mimeType = mimeExp.value();
    return r;
}

JsonString PromptArgument::toJson() const {
    JsonWriter writer;
    writer.startObject();
    writer.key("name");
    writer.string(name);
    writer.key("description");
    writer.string(description);
    writer.key("required");
    writer.boolean(required);
    writer.endObject();
    return writer.takeString();
}

std::expected<PromptArgument, McpError> PromptArgument::fromJson(const JsonElement& element) {
    auto objExp = requireObject(element, "prompt argument");
    if (!objExp) {
        return std::unexpected(objExp.error());
    }
    JsonObject obj = objExp.value();

    PromptArgument arg;
    auto nameExp = requireString(obj, "name");
    if (!nameExp) {
        return std::unexpected(nameExp.error());
    }
    auto descExp = requireString(obj, "description");
    if (!descExp) {
        return std::unexpected(descExp.error());
    }
    arg.name = nameExp.value();
    arg.description = descExp.value();

    bool required = false;
    if (JsonHelper::getBool(obj, "required", required)) {
        arg.required = required;
    }

    return arg;
}

JsonString Prompt::toJson() const {
    JsonWriter writer;
    writer.startObject();
    writer.key("name");
    writer.string(name);
    writer.key("description");
    writer.string(description);
    writer.key("arguments");
    writer.startArray();
    for (const auto& arg : arguments) {
        writer.raw(arg.toJson());
    }
    writer.endArray();
    writer.endObject();
    return writer.takeString();
}

std::expected<Prompt, McpError> Prompt::fromJson(const JsonElement& element) {
    auto objExp = requireObject(element, "prompt");
    if (!objExp) {
        return std::unexpected(objExp.error());
    }
    JsonObject obj = objExp.value();

    Prompt p;
    auto nameExp = requireString(obj, "name");
    if (!nameExp) {
        return std::unexpected(nameExp.error());
    }
    auto descExp = requireString(obj, "description");
    if (!descExp) {
        return std::unexpected(descExp.error());
    }
    p.name = nameExp.value();
    p.description = descExp.value();

    JsonArray argsArray;
    if (JsonHelper::getArray(obj, "arguments", argsArray)) {
        for (auto item : argsArray) {
            auto argExp = PromptArgument::fromJson(item);
            if (!argExp) {
                return std::unexpected(argExp.error());
            }
            p.arguments.push_back(std::move(argExp.value()));
        }
    }

    return p;
}

JsonString ClientInfo::toJson() const {
    JsonWriter writer;
    writer.startObject();
    writer.key("name");
    writer.string(name);
    writer.key("version");
    writer.string(version);
    writer.endObject();
    return writer.takeString();
}

std::expected<ClientInfo, McpError> ClientInfo::fromJson(const JsonElement& element) {
    auto objExp = requireObject(element, "clientInfo");
    if (!objExp) {
        return std::unexpected(objExp.error());
    }
    JsonObject obj = objExp.value();

    ClientInfo c;
    auto nameExp = requireString(obj, "name");
    if (!nameExp) {
        return std::unexpected(nameExp.error());
    }
    auto versionExp = requireString(obj, "version");
    if (!versionExp) {
        return std::unexpected(versionExp.error());
    }
    c.name = nameExp.value();
    c.version = versionExp.value();
    return c;
}

JsonString ServerInfo::toJson() const {
    JsonWriter writer;
    writer.startObject();
    writer.key("name");
    writer.string(name);
    writer.key("version");
    writer.string(version);
    writer.key("capabilities");
    writeRawOrEmptyObject(writer, capabilities);
    writer.endObject();
    return writer.takeString();
}

std::expected<ServerInfo, McpError> ServerInfo::fromJson(const JsonElement& element) {
    auto objExp = requireObject(element, "serverInfo");
    if (!objExp) {
        return std::unexpected(objExp.error());
    }
    JsonObject obj = objExp.value();

    ServerInfo s;
    auto nameExp = requireString(obj, "name");
    if (!nameExp) {
        return std::unexpected(nameExp.error());
    }
    auto versionExp = requireString(obj, "version");
    if (!versionExp) {
        return std::unexpected(versionExp.error());
    }
    s.name = nameExp.value();
    s.version = versionExp.value();

    JsonElement capsElement;
    if (JsonHelper::getElement(obj, "capabilities", capsElement)) {
        std::string raw;
        if (JsonHelper::getRawJson(capsElement, raw)) {
            s.capabilities = std::move(raw);
        }
    }

    return s;
}

JsonString ServerCapabilities::toJson() const {
    JsonWriter writer;
    writer.startObject();
    if (tools) {
        writer.key("tools");
        writer.startObject();
        writer.endObject();
    }
    if (resources) {
        writer.key("resources");
        writer.startObject();
        writer.endObject();
    }
    if (prompts) {
        writer.key("prompts");
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

std::expected<ServerCapabilities, McpError> ServerCapabilities::fromJson(const JsonElement& element) {
    auto objExp = requireObject(element, "capabilities");
    if (!objExp) {
        return std::unexpected(objExp.error());
    }
    JsonObject obj = objExp.value();

    ServerCapabilities c;
    auto toolsVal = obj["tools"];
    c.tools = !toolsVal.error() && !toolsVal.is_null();
    auto resVal = obj["resources"];
    c.resources = !resVal.error() && !resVal.is_null();
    auto promptsVal = obj["prompts"];
    c.prompts = !promptsVal.error() && !promptsVal.is_null();
    auto loggingVal = obj["logging"];
    c.logging = !loggingVal.error() && !loggingVal.is_null();

    return c;
}

JsonString InitializeParams::toJson() const {
    JsonWriter writer;
    writer.startObject();
    writer.key("protocolVersion");
    writer.string(protocolVersion);
    writer.key("clientInfo");
    writer.raw(clientInfo.toJson());
    writer.key("capabilities");
    writeRawOrEmptyObject(writer, capabilities);
    writer.endObject();
    return writer.takeString();
}

std::expected<InitializeParams, McpError> InitializeParams::fromJson(const JsonElement& element) {
    auto objExp = requireObject(element, "initialize params");
    if (!objExp) {
        return std::unexpected(objExp.error());
    }
    JsonObject obj = objExp.value();

    InitializeParams p;
    auto protocolExp = requireString(obj, "protocolVersion");
    if (!protocolExp) {
        return std::unexpected(protocolExp.error());
    }
    p.protocolVersion = protocolExp.value();

    JsonElement clientElement;
    if (!JsonHelper::getElement(obj, "clientInfo", clientElement)) {
        return std::unexpected(McpError::invalidMessage("Missing clientInfo"));
    }
    auto clientExp = ClientInfo::fromJson(clientElement);
    if (!clientExp) {
        return std::unexpected(clientExp.error());
    }
    p.clientInfo = std::move(clientExp.value());

    JsonElement capsElement;
    if (JsonHelper::getElement(obj, "capabilities", capsElement)) {
        std::string raw;
        if (JsonHelper::getRawJson(capsElement, raw)) {
            p.capabilities = std::move(raw);
        }
    }

    return p;
}

JsonString InitializeResult::toJson() const {
    JsonWriter writer;
    writer.startObject();
    writer.key("protocolVersion");
    writer.string(protocolVersion);
    writer.key("serverInfo");
    writer.raw(serverInfo.toJson());
    writer.key("capabilities");
    writer.raw(capabilities.toJson());
    writer.endObject();
    return writer.takeString();
}

std::expected<InitializeResult, McpError> InitializeResult::fromJson(const JsonElement& element) {
    auto objExp = requireObject(element, "initialize result");
    if (!objExp) {
        return std::unexpected(objExp.error());
    }
    JsonObject obj = objExp.value();

    InitializeResult r;
    auto protocolExp = requireString(obj, "protocolVersion");
    if (!protocolExp) {
        return std::unexpected(protocolExp.error());
    }
    r.protocolVersion = protocolExp.value();

    JsonElement serverElement;
    if (!JsonHelper::getElement(obj, "serverInfo", serverElement)) {
        return std::unexpected(McpError::invalidMessage("Missing serverInfo"));
    }
    auto serverExp = ServerInfo::fromJson(serverElement);
    if (!serverExp) {
        return std::unexpected(serverExp.error());
    }
    r.serverInfo = std::move(serverExp.value());

    JsonElement capsElement;
    if (!JsonHelper::getElement(obj, "capabilities", capsElement)) {
        return std::unexpected(McpError::invalidMessage("Missing capabilities"));
    }
    auto capsExp = ServerCapabilities::fromJson(capsElement);
    if (!capsExp) {
        return std::unexpected(capsExp.error());
    }
    r.capabilities = std::move(capsExp.value());

    return r;
}

JsonString ToolCallParams::toJson() const {
    JsonWriter writer;
    writer.startObject();
    writer.key("name");
    writer.string(name);
    writer.key("arguments");
    writeRawOrEmptyObject(writer, arguments);
    writer.endObject();
    return writer.takeString();
}

std::expected<ToolCallParams, McpError> ToolCallParams::fromJson(const JsonElement& element) {
    auto objExp = requireObject(element, "tool call params");
    if (!objExp) {
        return std::unexpected(objExp.error());
    }
    JsonObject obj = objExp.value();

    ToolCallParams p;
    auto nameExp = requireString(obj, "name");
    if (!nameExp) {
        return std::unexpected(nameExp.error());
    }
    p.name = nameExp.value();

    JsonElement argsElement;
    if (JsonHelper::getElement(obj, "arguments", argsElement)) {
        std::string raw;
        if (JsonHelper::getRawJson(argsElement, raw)) {
            p.arguments = std::move(raw);
        }
    }

    return p;
}

JsonString ToolCallResult::toJson() const {
    JsonWriter writer;
    writer.startObject();
    writer.key("content");
    writer.startArray();
    for (const auto& item : content) {
        writer.raw(item.toJson());
    }
    writer.endArray();
    if (isError) {
        writer.key("isError");
        writer.boolean(true);
    }
    writer.endObject();
    return writer.takeString();
}

std::expected<ToolCallResult, McpError> ToolCallResult::fromJson(const JsonElement& element) {
    auto objExp = requireObject(element, "tool call result");
    if (!objExp) {
        return std::unexpected(objExp.error());
    }
    JsonObject obj = objExp.value();

    ToolCallResult r;

    JsonArray contentArray;
    if (JsonHelper::getArray(obj, "content", contentArray)) {
        for (auto item : contentArray) {
            auto contentExp = Content::fromJson(item);
            if (!contentExp) {
                return std::unexpected(contentExp.error());
            }
            r.content.push_back(std::move(contentExp.value()));
        }
    }

    bool isError = false;
    if (JsonHelper::getBool(obj, "isError", isError)) {
        r.isError = isError;
    }

    return r;
}

JsonString JsonRpcRequest::toJson() const {
    JsonWriter writer;
    writer.startObject();
    writer.key("jsonrpc");
    writer.string(jsonrpc);
    if (id.has_value()) {
        writer.key("id");
        writer.number(id.value());
    }
    writer.key("method");
    writer.string(method);
    if (params.has_value()) {
        writer.key("params");
        writeRawOrEmptyObject(writer, params.value());
    }
    writer.endObject();
    return writer.takeString();
}

JsonString JsonRpcResponse::toJson() const {
    JsonWriter writer;
    writer.startObject();
    writer.key("jsonrpc");
    writer.string(jsonrpc);
    writer.key("id");
    writer.number(id);
    if (result.has_value()) {
        writer.key("result");
        writeRawOrEmptyObject(writer, result.value());
    }
    if (error.has_value()) {
        writer.key("error");
        writeRawOrEmptyObject(writer, error.value());
    }
    writer.endObject();
    return writer.takeString();
}

std::expected<JsonRpcResponse, McpError> JsonRpcResponse::fromJson(const JsonElement& element) {
    auto objExp = requireObject(element, "jsonrpc response");
    if (!objExp) {
        return std::unexpected(objExp.error());
    }
    JsonObject obj = objExp.value();

    JsonRpcResponse r;
    auto idExp = requireInt64(obj, "id");
    if (!idExp) {
        return std::unexpected(idExp.error());
    }
    r.id = idExp.value();

    JsonElement resultElement;
    if (JsonHelper::getElement(obj, "result", resultElement)) {
        std::string raw;
        if (JsonHelper::getRawJson(resultElement, raw)) {
            r.result = std::move(raw);
        }
    }

    JsonElement errorElement;
    if (JsonHelper::getElement(obj, "error", errorElement)) {
        std::string raw;
        if (JsonHelper::getRawJson(errorElement, raw)) {
            r.error = std::move(raw);
        }
    }

    return r;
}

JsonString JsonRpcNotification::toJson() const {
    JsonWriter writer;
    writer.startObject();
    writer.key("jsonrpc");
    writer.string(jsonrpc);
    writer.key("method");
    writer.string(method);
    if (params.has_value()) {
        writer.key("params");
        writeRawOrEmptyObject(writer, params.value());
    }
    writer.endObject();
    return writer.takeString();
}

JsonString JsonRpcError::toJson() const {
    JsonWriter writer;
    writer.startObject();
    writer.key("code");
    writer.number(static_cast<int64_t>(code));
    writer.key("message");
    writer.string(message);
    if (data.has_value()) {
        writer.key("data");
        writeRawOrEmptyObject(writer, data.value());
    }
    writer.endObject();
    return writer.takeString();
}

std::expected<JsonRpcError, McpError> JsonRpcError::fromJson(const JsonElement& element) {
    auto objExp = requireObject(element, "jsonrpc error");
    if (!objExp) {
        return std::unexpected(objExp.error());
    }
    JsonObject obj = objExp.value();

    JsonRpcError e;
    auto codeExp = requireInt64(obj, "code");
    if (!codeExp) {
        return std::unexpected(codeExp.error());
    }
    e.code = static_cast<int>(codeExp.value());
    auto msgExp = requireString(obj, "message");
    if (!msgExp) {
        return std::unexpected(msgExp.error());
    }
    e.message = msgExp.value();

    JsonElement dataElement;
    if (JsonHelper::getElement(obj, "data", dataElement)) {
        std::string raw;
        if (JsonHelper::getRawJson(dataElement, raw)) {
            e.data = std::move(raw);
        }
    }

    return e;
}

} // namespace mcp
} // namespace galay
