#include <galay/cpp/galay-mcp/common/mcp_base.h>

namespace galay {
namespace mcp {

namespace {

std::expected<JsonObject, McpError> RequireObject(const JsonElement& element, const char* context) {
    JsonObject obj;
    if (!JsonHelper::GetObject(element, obj)) {
        return std::unexpected(McpError::invalidMessage(std::string("Expected object for ") + context));
    }
    return obj;
}

std::expected<std::string, McpError> RequireString(const JsonObject& obj, const char* key) {
    std::string value;
    if (!JsonHelper::GetString(obj, key, value)) {
        return std::unexpected(McpError::invalidMessage(std::string("Missing or invalid ") + key));
    }
    return value;
}

std::expected<int64_t, McpError> RequireInt64(const JsonObject& obj, const char* key) {
    int64_t value = 0;
    if (!JsonHelper::GetInt64(obj, key, value)) {
        return std::unexpected(McpError::invalidMessage(std::string("Missing or invalid ") + key));
    }
    return value;
}

void WriteRawOrEmptyObject(JsonWriter& writer, const JsonString& raw) {
    if (raw.empty()) {
        writer.StartObject();
        writer.EndObject();
        return;
    }
    writer.Raw(raw);
}

} // namespace

JsonString Content::toJson() const {
    JsonWriter writer;
    writer.StartObject();
    switch (type) {
        case ContentType::Text:
            writer.Key("type");
            writer.String("text");
            writer.Key("text");
            writer.String(text);
            break;
        case ContentType::Image:
            writer.Key("type");
            writer.String("image");
            writer.Key("data");
            writer.String(data);
            writer.Key("mimeType");
            writer.String(mimeType);
            break;
        case ContentType::Resource:
            writer.Key("type");
            writer.String("resource");
            writer.Key("uri");
            writer.String(uri);
            break;
    }
    writer.EndObject();
    return writer.TakeString();
}

std::expected<Content, McpError> Content::fromJson(const JsonElement& element) {
    auto objExp = RequireObject(element, "content");
    if (!objExp) {
        return std::unexpected(objExp.error());
    }
    JsonObject obj = objExp.value();

    auto typeStrExp = RequireString(obj, "type");
    if (!typeStrExp) {
        return std::unexpected(typeStrExp.error());
    }

    Content c;
    const std::string& typeStr = typeStrExp.value();
    if (typeStr == "text") {
        c.type = ContentType::Text;
        auto textExp = RequireString(obj, "text");
        if (!textExp) {
            return std::unexpected(textExp.error());
        }
        c.text = textExp.value();
    } else if (typeStr == "image") {
        c.type = ContentType::Image;
        auto dataExp = RequireString(obj, "data");
        if (!dataExp) {
            return std::unexpected(dataExp.error());
        }
        auto mimeExp = RequireString(obj, "mimeType");
        if (!mimeExp) {
            return std::unexpected(mimeExp.error());
        }
        c.data = dataExp.value();
        c.mimeType = mimeExp.value();
    } else if (typeStr == "resource") {
        c.type = ContentType::Resource;
        auto uriExp = RequireString(obj, "uri");
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
    writer.StartObject();
    writer.Key("name");
    writer.String(name);
    writer.Key("description");
    writer.String(description);
    writer.Key("inputSchema");
    WriteRawOrEmptyObject(writer, inputSchema);
    writer.EndObject();
    return writer.TakeString();
}

std::expected<Tool, McpError> Tool::fromJson(const JsonElement& element) {
    auto objExp = RequireObject(element, "tool");
    if (!objExp) {
        return std::unexpected(objExp.error());
    }
    JsonObject obj = objExp.value();

    Tool t;
    auto nameExp = RequireString(obj, "name");
    if (!nameExp) {
        return std::unexpected(nameExp.error());
    }
    auto descExp = RequireString(obj, "description");
    if (!descExp) {
        return std::unexpected(descExp.error());
    }
    t.name = nameExp.value();
    t.description = descExp.value();

    JsonElement schemaElement;
    if (JsonHelper::GetElement(obj, "inputSchema", schemaElement)) {
        std::string raw;
        if (JsonHelper::GetRawJson(schemaElement, raw)) {
            t.inputSchema = std::move(raw);
        }
    }

    return t;
}

JsonString Resource::toJson() const {
    JsonWriter writer;
    writer.StartObject();
    writer.Key("uri");
    writer.String(uri);
    writer.Key("name");
    writer.String(name);
    writer.Key("description");
    writer.String(description);
    writer.Key("mimeType");
    writer.String(mimeType);
    writer.EndObject();
    return writer.TakeString();
}

std::expected<Resource, McpError> Resource::fromJson(const JsonElement& element) {
    auto objExp = RequireObject(element, "resource");
    if (!objExp) {
        return std::unexpected(objExp.error());
    }
    JsonObject obj = objExp.value();

    Resource r;
    auto uriExp = RequireString(obj, "uri");
    if (!uriExp) {
        return std::unexpected(uriExp.error());
    }
    auto nameExp = RequireString(obj, "name");
    if (!nameExp) {
        return std::unexpected(nameExp.error());
    }
    auto descExp = RequireString(obj, "description");
    if (!descExp) {
        return std::unexpected(descExp.error());
    }
    auto mimeExp = RequireString(obj, "mimeType");
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
    writer.StartObject();
    writer.Key("name");
    writer.String(name);
    writer.Key("description");
    writer.String(description);
    writer.Key("required");
    writer.Bool(required);
    writer.EndObject();
    return writer.TakeString();
}

std::expected<PromptArgument, McpError> PromptArgument::fromJson(const JsonElement& element) {
    auto objExp = RequireObject(element, "prompt argument");
    if (!objExp) {
        return std::unexpected(objExp.error());
    }
    JsonObject obj = objExp.value();

    PromptArgument arg;
    auto nameExp = RequireString(obj, "name");
    if (!nameExp) {
        return std::unexpected(nameExp.error());
    }
    auto descExp = RequireString(obj, "description");
    if (!descExp) {
        return std::unexpected(descExp.error());
    }
    arg.name = nameExp.value();
    arg.description = descExp.value();

    bool required = false;
    if (JsonHelper::GetBool(obj, "required", required)) {
        arg.required = required;
    }

    return arg;
}

JsonString Prompt::toJson() const {
    JsonWriter writer;
    writer.StartObject();
    writer.Key("name");
    writer.String(name);
    writer.Key("description");
    writer.String(description);
    writer.Key("arguments");
    writer.StartArray();
    for (const auto& arg : arguments) {
        writer.Raw(arg.toJson());
    }
    writer.EndArray();
    writer.EndObject();
    return writer.TakeString();
}

std::expected<Prompt, McpError> Prompt::fromJson(const JsonElement& element) {
    auto objExp = RequireObject(element, "prompt");
    if (!objExp) {
        return std::unexpected(objExp.error());
    }
    JsonObject obj = objExp.value();

    Prompt p;
    auto nameExp = RequireString(obj, "name");
    if (!nameExp) {
        return std::unexpected(nameExp.error());
    }
    auto descExp = RequireString(obj, "description");
    if (!descExp) {
        return std::unexpected(descExp.error());
    }
    p.name = nameExp.value();
    p.description = descExp.value();

    JsonArray argsArray;
    if (JsonHelper::GetArray(obj, "arguments", argsArray)) {
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
    writer.StartObject();
    writer.Key("name");
    writer.String(name);
    writer.Key("version");
    writer.String(version);
    writer.EndObject();
    return writer.TakeString();
}

std::expected<ClientInfo, McpError> ClientInfo::fromJson(const JsonElement& element) {
    auto objExp = RequireObject(element, "clientInfo");
    if (!objExp) {
        return std::unexpected(objExp.error());
    }
    JsonObject obj = objExp.value();

    ClientInfo c;
    auto nameExp = RequireString(obj, "name");
    if (!nameExp) {
        return std::unexpected(nameExp.error());
    }
    auto versionExp = RequireString(obj, "version");
    if (!versionExp) {
        return std::unexpected(versionExp.error());
    }
    c.name = nameExp.value();
    c.version = versionExp.value();
    return c;
}

JsonString ServerInfo::toJson() const {
    JsonWriter writer;
    writer.StartObject();
    writer.Key("name");
    writer.String(name);
    writer.Key("version");
    writer.String(version);
    writer.Key("capabilities");
    WriteRawOrEmptyObject(writer, capabilities);
    writer.EndObject();
    return writer.TakeString();
}

std::expected<ServerInfo, McpError> ServerInfo::fromJson(const JsonElement& element) {
    auto objExp = RequireObject(element, "serverInfo");
    if (!objExp) {
        return std::unexpected(objExp.error());
    }
    JsonObject obj = objExp.value();

    ServerInfo s;
    auto nameExp = RequireString(obj, "name");
    if (!nameExp) {
        return std::unexpected(nameExp.error());
    }
    auto versionExp = RequireString(obj, "version");
    if (!versionExp) {
        return std::unexpected(versionExp.error());
    }
    s.name = nameExp.value();
    s.version = versionExp.value();

    JsonElement capsElement;
    if (JsonHelper::GetElement(obj, "capabilities", capsElement)) {
        std::string raw;
        if (JsonHelper::GetRawJson(capsElement, raw)) {
            s.capabilities = std::move(raw);
        }
    }

    return s;
}

JsonString ServerCapabilities::toJson() const {
    JsonWriter writer;
    writer.StartObject();
    if (tools) {
        writer.Key("tools");
        writer.StartObject();
        writer.EndObject();
    }
    if (resources) {
        writer.Key("resources");
        writer.StartObject();
        writer.EndObject();
    }
    if (prompts) {
        writer.Key("prompts");
        writer.StartObject();
        writer.EndObject();
    }
    if (logging) {
        writer.Key("logging");
        writer.StartObject();
        writer.EndObject();
    }
    writer.EndObject();
    return writer.TakeString();
}

std::expected<ServerCapabilities, McpError> ServerCapabilities::fromJson(const JsonElement& element) {
    auto objExp = RequireObject(element, "capabilities");
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
    writer.StartObject();
    writer.Key("protocolVersion");
    writer.String(protocolVersion);
    writer.Key("clientInfo");
    writer.Raw(clientInfo.toJson());
    writer.Key("capabilities");
    WriteRawOrEmptyObject(writer, capabilities);
    writer.EndObject();
    return writer.TakeString();
}

std::expected<InitializeParams, McpError> InitializeParams::fromJson(const JsonElement& element) {
    auto objExp = RequireObject(element, "initialize params");
    if (!objExp) {
        return std::unexpected(objExp.error());
    }
    JsonObject obj = objExp.value();

    InitializeParams p;
    auto protocolExp = RequireString(obj, "protocolVersion");
    if (!protocolExp) {
        return std::unexpected(protocolExp.error());
    }
    p.protocolVersion = protocolExp.value();

    JsonElement clientElement;
    if (!JsonHelper::GetElement(obj, "clientInfo", clientElement)) {
        return std::unexpected(McpError::invalidMessage("Missing clientInfo"));
    }
    auto clientExp = ClientInfo::fromJson(clientElement);
    if (!clientExp) {
        return std::unexpected(clientExp.error());
    }
    p.clientInfo = std::move(clientExp.value());

    JsonElement capsElement;
    if (JsonHelper::GetElement(obj, "capabilities", capsElement)) {
        std::string raw;
        if (JsonHelper::GetRawJson(capsElement, raw)) {
            p.capabilities = std::move(raw);
        }
    }

    return p;
}

JsonString InitializeResult::toJson() const {
    JsonWriter writer;
    writer.StartObject();
    writer.Key("protocolVersion");
    writer.String(protocolVersion);
    writer.Key("serverInfo");
    writer.Raw(serverInfo.toJson());
    writer.Key("capabilities");
    writer.Raw(capabilities.toJson());
    writer.EndObject();
    return writer.TakeString();
}

std::expected<InitializeResult, McpError> InitializeResult::fromJson(const JsonElement& element) {
    auto objExp = RequireObject(element, "initialize result");
    if (!objExp) {
        return std::unexpected(objExp.error());
    }
    JsonObject obj = objExp.value();

    InitializeResult r;
    auto protocolExp = RequireString(obj, "protocolVersion");
    if (!protocolExp) {
        return std::unexpected(protocolExp.error());
    }
    r.protocolVersion = protocolExp.value();

    JsonElement serverElement;
    if (!JsonHelper::GetElement(obj, "serverInfo", serverElement)) {
        return std::unexpected(McpError::invalidMessage("Missing serverInfo"));
    }
    auto serverExp = ServerInfo::fromJson(serverElement);
    if (!serverExp) {
        return std::unexpected(serverExp.error());
    }
    r.serverInfo = std::move(serverExp.value());

    JsonElement capsElement;
    if (!JsonHelper::GetElement(obj, "capabilities", capsElement)) {
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
    writer.StartObject();
    writer.Key("name");
    writer.String(name);
    writer.Key("arguments");
    WriteRawOrEmptyObject(writer, arguments);
    writer.EndObject();
    return writer.TakeString();
}

std::expected<ToolCallParams, McpError> ToolCallParams::fromJson(const JsonElement& element) {
    auto objExp = RequireObject(element, "tool call params");
    if (!objExp) {
        return std::unexpected(objExp.error());
    }
    JsonObject obj = objExp.value();

    ToolCallParams p;
    auto nameExp = RequireString(obj, "name");
    if (!nameExp) {
        return std::unexpected(nameExp.error());
    }
    p.name = nameExp.value();

    JsonElement argsElement;
    if (JsonHelper::GetElement(obj, "arguments", argsElement)) {
        std::string raw;
        if (JsonHelper::GetRawJson(argsElement, raw)) {
            p.arguments = std::move(raw);
        }
    }

    return p;
}

JsonString ToolCallResult::toJson() const {
    JsonWriter writer;
    writer.StartObject();
    writer.Key("content");
    writer.StartArray();
    for (const auto& item : content) {
        writer.Raw(item.toJson());
    }
    writer.EndArray();
    if (isError) {
        writer.Key("isError");
        writer.Bool(true);
    }
    writer.EndObject();
    return writer.TakeString();
}

std::expected<ToolCallResult, McpError> ToolCallResult::fromJson(const JsonElement& element) {
    auto objExp = RequireObject(element, "tool call result");
    if (!objExp) {
        return std::unexpected(objExp.error());
    }
    JsonObject obj = objExp.value();

    ToolCallResult r;

    JsonArray contentArray;
    if (JsonHelper::GetArray(obj, "content", contentArray)) {
        for (auto item : contentArray) {
            auto contentExp = Content::fromJson(item);
            if (!contentExp) {
                return std::unexpected(contentExp.error());
            }
            r.content.push_back(std::move(contentExp.value()));
        }
    }

    bool isError = false;
    if (JsonHelper::GetBool(obj, "isError", isError)) {
        r.isError = isError;
    }

    return r;
}

JsonString JsonRpcRequest::toJson() const {
    JsonWriter writer;
    writer.StartObject();
    writer.Key("jsonrpc");
    writer.String(jsonrpc);
    if (id.has_value()) {
        writer.Key("id");
        writer.Number(id.value());
    }
    writer.Key("method");
    writer.String(method);
    if (params.has_value()) {
        writer.Key("params");
        WriteRawOrEmptyObject(writer, params.value());
    }
    writer.EndObject();
    return writer.TakeString();
}

JsonString JsonRpcResponse::toJson() const {
    JsonWriter writer;
    writer.StartObject();
    writer.Key("jsonrpc");
    writer.String(jsonrpc);
    writer.Key("id");
    writer.Number(id);
    if (result.has_value()) {
        writer.Key("result");
        WriteRawOrEmptyObject(writer, result.value());
    }
    if (error.has_value()) {
        writer.Key("error");
        WriteRawOrEmptyObject(writer, error.value());
    }
    writer.EndObject();
    return writer.TakeString();
}

std::expected<JsonRpcResponse, McpError> JsonRpcResponse::fromJson(const JsonElement& element) {
    auto objExp = RequireObject(element, "jsonrpc response");
    if (!objExp) {
        return std::unexpected(objExp.error());
    }
    JsonObject obj = objExp.value();

    JsonRpcResponse r;
    auto idExp = RequireInt64(obj, "id");
    if (!idExp) {
        return std::unexpected(idExp.error());
    }
    r.id = idExp.value();

    JsonElement resultElement;
    if (JsonHelper::GetElement(obj, "result", resultElement)) {
        std::string raw;
        if (JsonHelper::GetRawJson(resultElement, raw)) {
            r.result = std::move(raw);
        }
    }

    JsonElement errorElement;
    if (JsonHelper::GetElement(obj, "error", errorElement)) {
        std::string raw;
        if (JsonHelper::GetRawJson(errorElement, raw)) {
            r.error = std::move(raw);
        }
    }

    return r;
}

JsonString JsonRpcNotification::toJson() const {
    JsonWriter writer;
    writer.StartObject();
    writer.Key("jsonrpc");
    writer.String(jsonrpc);
    writer.Key("method");
    writer.String(method);
    if (params.has_value()) {
        writer.Key("params");
        WriteRawOrEmptyObject(writer, params.value());
    }
    writer.EndObject();
    return writer.TakeString();
}

JsonString JsonRpcError::toJson() const {
    JsonWriter writer;
    writer.StartObject();
    writer.Key("code");
    writer.Number(static_cast<int64_t>(code));
    writer.Key("message");
    writer.String(message);
    if (data.has_value()) {
        writer.Key("data");
        WriteRawOrEmptyObject(writer, data.value());
    }
    writer.EndObject();
    return writer.TakeString();
}

std::expected<JsonRpcError, McpError> JsonRpcError::fromJson(const JsonElement& element) {
    auto objExp = RequireObject(element, "jsonrpc error");
    if (!objExp) {
        return std::unexpected(objExp.error());
    }
    JsonObject obj = objExp.value();

    JsonRpcError e;
    auto codeExp = RequireInt64(obj, "code");
    if (!codeExp) {
        return std::unexpected(codeExp.error());
    }
    e.code = static_cast<int>(codeExp.value());
    auto msgExp = RequireString(obj, "message");
    if (!msgExp) {
        return std::unexpected(msgExp.error());
    }
    e.message = msgExp.value();

    JsonElement dataElement;
    if (JsonHelper::GetElement(obj, "data", dataElement)) {
        std::string raw;
        if (JsonHelper::GetRawJson(dataElement, raw)) {
            e.data = std::move(raw);
        }
    }

    return e;
}

} // namespace mcp
} // namespace galay
