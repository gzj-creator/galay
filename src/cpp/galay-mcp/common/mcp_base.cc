#include "mcp_base.h"

namespace galay::mcp {

namespace {

std::expected<json::Json, McpError> require_object(const json::Json& element, const char* context) {
    if (!element.is_object()) {
        return std::unexpected(McpError::invalid_message(std::string("Expected object for ") + context));
    }
    return element;
}

std::expected<std::string, McpError> require_string(const json::Json& obj, const char* key) {
    auto value = obj.at(key).as_string();
    if (!value) {
        return std::unexpected(McpError::invalid_message(std::string("Missing or invalid ") + key));
    }
    return std::string(*value);
}

std::expected<int64_t, McpError> require_int64(const json::Json& obj, const char* key) {
    auto value = obj.at(key).as_int64();
    if (!value) {
        return std::unexpected(McpError::invalid_message(std::string("Missing or invalid ") + key));
    }
    return *value;
}

void write_raw_or_empty_object(json::stream::StreamWriter& writer, const std::string& raw) {
    // StreamWriter 失败粘滞：中间结果统一丢弃，由 finish() 统一检查
    if (raw.empty()) {
        (void)writer.start_object();
        (void)writer.end_object();
        return;
    }
    (void)writer.raw(raw);
}

} // namespace

std::string Content::to_json() const {
    std::string out;
    auto writer = make_json_writer(out);
    // StreamWriter 失败粘滞：中间结果统一丢弃，由 finish() 统一检查
    (void)writer.start_object();
    switch (type) {
        case ContentType::Text:
            (void)writer.key("type");
            (void)writer.string("text");
            (void)writer.key("text");
            (void)writer.string(text);
            break;
        case ContentType::Image:
            (void)writer.key("type");
            (void)writer.string("image");
            (void)writer.key("data");
            (void)writer.string(data);
            (void)writer.key("mimeType");
            (void)writer.string(mimeType);
            break;
        case ContentType::Resource:
            (void)writer.key("type");
            (void)writer.string("resource");
            (void)writer.key("uri");
            (void)writer.string(uri);
            break;
    }
    (void)writer.end_object();
    if (!writer.finish()) {
        return std::string{};
    }
    return std::move(out);
}

std::expected<Content, McpError> Content::from_json(const json::Json& element) {
    auto objExp = require_object(element, "content");
    if (!objExp) {
        return std::unexpected(objExp.error());
    }
    json::Json obj = objExp.value();

    auto typeStrExp = require_string(obj, "type");
    if (!typeStrExp) {
        return std::unexpected(typeStrExp.error());
    }

    Content c;
    const std::string& typeStr = typeStrExp.value();
    if (typeStr == "text") {
        c.type = ContentType::Text;
        auto textExp = require_string(obj, "text");
        if (!textExp) {
            return std::unexpected(textExp.error());
        }
        c.text = textExp.value();
    } else if (typeStr == "image") {
        c.type = ContentType::Image;
        auto dataExp = require_string(obj, "data");
        if (!dataExp) {
            return std::unexpected(dataExp.error());
        }
        auto mimeExp = require_string(obj, "mimeType");
        if (!mimeExp) {
            return std::unexpected(mimeExp.error());
        }
        c.data = dataExp.value();
        c.mimeType = mimeExp.value();
    } else if (typeStr == "resource") {
        c.type = ContentType::Resource;
        auto uriExp = require_string(obj, "uri");
        if (!uriExp) {
            return std::unexpected(uriExp.error());
        }
        c.uri = uriExp.value();
    } else {
        return std::unexpected(McpError::invalid_message("Unknown content type"));
    }

    return c;
}

std::string Tool::to_json() const {
    std::string out;
    auto writer = make_json_writer(out);
    // StreamWriter 失败粘滞：中间结果统一丢弃，由 finish() 统一检查
    (void)writer.start_object();
    (void)writer.key("name");
    (void)writer.string(name);
    (void)writer.key("description");
    (void)writer.string(description);
    (void)writer.key("inputSchema");
    write_raw_or_empty_object(writer, inputSchema);
    (void)writer.end_object();
    if (!writer.finish()) {
        return std::string{};
    }
    return std::move(out);
}

std::expected<Tool, McpError> Tool::from_json(const json::Json& element) {
    auto objExp = require_object(element, "tool");
    if (!objExp) {
        return std::unexpected(objExp.error());
    }
    json::Json obj = objExp.value();

    Tool t;
    auto nameExp = require_string(obj, "name");
    if (!nameExp) {
        return std::unexpected(nameExp.error());
    }
    auto descExp = require_string(obj, "description");
    if (!descExp) {
        return std::unexpected(descExp.error());
    }
    t.name = nameExp.value();
    t.description = descExp.value();

    json::Json schemaElement = obj.at("inputSchema");
    if (schemaElement.valid()) {
        std::string raw;
        auto serialized = json::stream::serialize(schemaElement, [&](std::string_view chunk) -> json::result<void> {
            raw.append(chunk);
            return {};
        });
        if (serialized) {
            t.inputSchema = std::move(raw);
        }
    }

    return t;
}

std::string Resource::to_json() const {
    std::string out;
    auto writer = make_json_writer(out);
    // StreamWriter 失败粘滞：中间结果统一丢弃，由 finish() 统一检查
    (void)writer.start_object();
    (void)writer.key("uri");
    (void)writer.string(uri);
    (void)writer.key("name");
    (void)writer.string(name);
    (void)writer.key("description");
    (void)writer.string(description);
    (void)writer.key("mimeType");
    (void)writer.string(mimeType);
    (void)writer.end_object();
    if (!writer.finish()) {
        return std::string{};
    }
    return std::move(out);
}

std::expected<Resource, McpError> Resource::from_json(const json::Json& element) {
    auto objExp = require_object(element, "resource");
    if (!objExp) {
        return std::unexpected(objExp.error());
    }
    json::Json obj = objExp.value();

    Resource r;
    auto uriExp = require_string(obj, "uri");
    if (!uriExp) {
        return std::unexpected(uriExp.error());
    }
    auto nameExp = require_string(obj, "name");
    if (!nameExp) {
        return std::unexpected(nameExp.error());
    }
    auto descExp = require_string(obj, "description");
    if (!descExp) {
        return std::unexpected(descExp.error());
    }
    auto mimeExp = require_string(obj, "mimeType");
    if (!mimeExp) {
        return std::unexpected(mimeExp.error());
    }

    r.uri = uriExp.value();
    r.name = nameExp.value();
    r.description = descExp.value();
    r.mimeType = mimeExp.value();
    return r;
}

std::string PromptArgument::to_json() const {
    std::string out;
    auto writer = make_json_writer(out);
    // StreamWriter 失败粘滞：中间结果统一丢弃，由 finish() 统一检查
    (void)writer.start_object();
    (void)writer.key("name");
    (void)writer.string(name);
    (void)writer.key("description");
    (void)writer.string(description);
    (void)writer.key("required");
    (void)writer.boolean(required);
    (void)writer.end_object();
    if (!writer.finish()) {
        return std::string{};
    }
    return std::move(out);
}

std::expected<PromptArgument, McpError> PromptArgument::from_json(const json::Json& element) {
    auto objExp = require_object(element, "prompt argument");
    if (!objExp) {
        return std::unexpected(objExp.error());
    }
    json::Json obj = objExp.value();

    PromptArgument arg;
    auto nameExp = require_string(obj, "name");
    if (!nameExp) {
        return std::unexpected(nameExp.error());
    }
    auto descExp = require_string(obj, "description");
    if (!descExp) {
        return std::unexpected(descExp.error());
    }
    arg.name = nameExp.value();
    arg.description = descExp.value();

    auto requiredVal = obj.at("required").as_bool();
    if (requiredVal) {
        arg.required = *requiredVal;
    }

    return arg;
}

std::string Prompt::to_json() const {
    std::string out;
    auto writer = make_json_writer(out);
    // StreamWriter 失败粘滞：中间结果统一丢弃，由 finish() 统一检查
    (void)writer.start_object();
    (void)writer.key("name");
    (void)writer.string(name);
    (void)writer.key("description");
    (void)writer.string(description);
    (void)writer.key("arguments");
    (void)writer.start_array();
    for (const auto& arg : arguments) {
        (void)writer.raw(arg.to_json());
    }
    (void)writer.end_array();
    (void)writer.end_object();
    if (!writer.finish()) {
        return std::string{};
    }
    return std::move(out);
}

std::expected<Prompt, McpError> Prompt::from_json(const json::Json& element) {
    auto objExp = require_object(element, "prompt");
    if (!objExp) {
        return std::unexpected(objExp.error());
    }
    json::Json obj = objExp.value();

    Prompt p;
    auto nameExp = require_string(obj, "name");
    if (!nameExp) {
        return std::unexpected(nameExp.error());
    }
    auto descExp = require_string(obj, "description");
    if (!descExp) {
        return std::unexpected(descExp.error());
    }
    p.name = nameExp.value();
    p.description = descExp.value();

    json::Json argsArray = obj.at("arguments");
    if (argsArray.is_array()) {
        for (size_t i = 0; i < argsArray.size(); ++i) {
            const json::Json item = argsArray.at(i);
            auto argExp = PromptArgument::from_json(item);
            if (!argExp) {
                return std::unexpected(argExp.error());
            }
            p.arguments.push_back(std::move(argExp.value()));
        }
    }

    return p;
}

std::string ClientInfo::to_json() const {
    std::string out;
    auto writer = make_json_writer(out);
    // StreamWriter 失败粘滞：中间结果统一丢弃，由 finish() 统一检查
    (void)writer.start_object();
    (void)writer.key("name");
    (void)writer.string(name);
    (void)writer.key("version");
    (void)writer.string(version);
    (void)writer.end_object();
    if (!writer.finish()) {
        return std::string{};
    }
    return std::move(out);
}

std::expected<ClientInfo, McpError> ClientInfo::from_json(const json::Json& element) {
    auto objExp = require_object(element, "clientInfo");
    if (!objExp) {
        return std::unexpected(objExp.error());
    }
    json::Json obj = objExp.value();

    ClientInfo c;
    auto nameExp = require_string(obj, "name");
    if (!nameExp) {
        return std::unexpected(nameExp.error());
    }
    auto versionExp = require_string(obj, "version");
    if (!versionExp) {
        return std::unexpected(versionExp.error());
    }
    c.name = nameExp.value();
    c.version = versionExp.value();
    return c;
}

std::string ServerInfo::to_json() const {
    std::string out;
    auto writer = make_json_writer(out);
    // StreamWriter 失败粘滞：中间结果统一丢弃，由 finish() 统一检查
    (void)writer.start_object();
    (void)writer.key("name");
    (void)writer.string(name);
    (void)writer.key("version");
    (void)writer.string(version);
    (void)writer.key("capabilities");
    write_raw_or_empty_object(writer, capabilities);
    (void)writer.end_object();
    if (!writer.finish()) {
        return std::string{};
    }
    return std::move(out);
}

std::expected<ServerInfo, McpError> ServerInfo::from_json(const json::Json& element) {
    auto objExp = require_object(element, "serverInfo");
    if (!objExp) {
        return std::unexpected(objExp.error());
    }
    json::Json obj = objExp.value();

    ServerInfo s;
    auto nameExp = require_string(obj, "name");
    if (!nameExp) {
        return std::unexpected(nameExp.error());
    }
    auto versionExp = require_string(obj, "version");
    if (!versionExp) {
        return std::unexpected(versionExp.error());
    }
    s.name = nameExp.value();
    s.version = versionExp.value();

    json::Json capsElement = obj.at("capabilities");
    if (capsElement.valid()) {
        std::string raw;
        auto serialized = json::stream::serialize(capsElement, [&](std::string_view chunk) -> json::result<void> {
            raw.append(chunk);
            return {};
        });
        if (serialized) {
            s.capabilities = std::move(raw);
        }
    }

    return s;
}

std::string ServerCapabilities::to_json() const {
    std::string out;
    auto writer = make_json_writer(out);
    // StreamWriter 失败粘滞：中间结果统一丢弃，由 finish() 统一检查
    (void)writer.start_object();
    if (tools) {
        (void)writer.key("tools");
        (void)writer.start_object();
        (void)writer.end_object();
    }
    if (resources) {
        (void)writer.key("resources");
        (void)writer.start_object();
        (void)writer.end_object();
    }
    if (prompts) {
        (void)writer.key("prompts");
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

std::expected<ServerCapabilities, McpError> ServerCapabilities::from_json(const json::Json& element) {
    auto objExp = require_object(element, "capabilities");
    if (!objExp) {
        return std::unexpected(objExp.error());
    }
    json::Json obj = objExp.value();

    ServerCapabilities c;
    auto toolsVal = obj["tools"];
    c.tools = toolsVal.valid() && !toolsVal.is_null();
    auto resVal = obj["resources"];
    c.resources = resVal.valid() && !resVal.is_null();
    auto promptsVal = obj["prompts"];
    c.prompts = promptsVal.valid() && !promptsVal.is_null();
    auto loggingVal = obj["logging"];
    c.logging = loggingVal.valid() && !loggingVal.is_null();

    return c;
}

std::string InitializeParams::to_json() const {
    std::string out;
    auto writer = make_json_writer(out);
    // StreamWriter 失败粘滞：中间结果统一丢弃，由 finish() 统一检查
    (void)writer.start_object();
    (void)writer.key("protocolVersion");
    (void)writer.string(protocolVersion);
    (void)writer.key("clientInfo");
    (void)writer.raw(clientInfo.to_json());
    (void)writer.key("capabilities");
    write_raw_or_empty_object(writer, capabilities);
    (void)writer.end_object();
    if (!writer.finish()) {
        return std::string{};
    }
    return std::move(out);
}

std::expected<InitializeParams, McpError> InitializeParams::from_json(const json::Json& element) {
    auto objExp = require_object(element, "initialize params");
    if (!objExp) {
        return std::unexpected(objExp.error());
    }
    json::Json obj = objExp.value();

    InitializeParams p;
    auto protocolExp = require_string(obj, "protocolVersion");
    if (!protocolExp) {
        return std::unexpected(protocolExp.error());
    }
    p.protocolVersion = protocolExp.value();

    json::Json clientElement = obj.at("clientInfo");
    if (!clientElement.valid()) {
        return std::unexpected(McpError::invalid_message("Missing clientInfo"));
    }
    auto clientExp = ClientInfo::from_json(clientElement);
    if (!clientExp) {
        return std::unexpected(clientExp.error());
    }
    p.clientInfo = std::move(clientExp.value());

    json::Json capsElement = obj.at("capabilities");
    if (capsElement.valid()) {
        std::string raw;
        auto serialized = json::stream::serialize(capsElement, [&](std::string_view chunk) -> json::result<void> {
            raw.append(chunk);
            return {};
        });
        if (serialized) {
            p.capabilities = std::move(raw);
        }
    }

    return p;
}

std::string InitializeResult::to_json() const {
    std::string out;
    auto writer = make_json_writer(out);
    // StreamWriter 失败粘滞：中间结果统一丢弃，由 finish() 统一检查
    (void)writer.start_object();
    (void)writer.key("protocolVersion");
    (void)writer.string(protocolVersion);
    (void)writer.key("serverInfo");
    (void)writer.raw(serverInfo.to_json());
    (void)writer.key("capabilities");
    (void)writer.raw(capabilities.to_json());
    (void)writer.end_object();
    if (!writer.finish()) {
        return std::string{};
    }
    return std::move(out);
}

std::expected<InitializeResult, McpError> InitializeResult::from_json(const json::Json& element) {
    auto objExp = require_object(element, "initialize result");
    if (!objExp) {
        return std::unexpected(objExp.error());
    }
    json::Json obj = objExp.value();

    InitializeResult r;
    auto protocolExp = require_string(obj, "protocolVersion");
    if (!protocolExp) {
        return std::unexpected(protocolExp.error());
    }
    r.protocolVersion = protocolExp.value();

    json::Json serverElement = obj.at("serverInfo");
    if (!serverElement.valid()) {
        return std::unexpected(McpError::invalid_message("Missing serverInfo"));
    }
    auto serverExp = ServerInfo::from_json(serverElement);
    if (!serverExp) {
        return std::unexpected(serverExp.error());
    }
    r.serverInfo = std::move(serverExp.value());

    json::Json capsElement = obj.at("capabilities");
    if (!capsElement.valid()) {
        return std::unexpected(McpError::invalid_message("Missing capabilities"));
    }
    auto capsExp = ServerCapabilities::from_json(capsElement);
    if (!capsExp) {
        return std::unexpected(capsExp.error());
    }
    r.capabilities = std::move(capsExp.value());

    return r;
}

std::string ToolCallParams::to_json() const {
    std::string out;
    auto writer = make_json_writer(out);
    // StreamWriter 失败粘滞：中间结果统一丢弃，由 finish() 统一检查
    (void)writer.start_object();
    (void)writer.key("name");
    (void)writer.string(name);
    (void)writer.key("arguments");
    write_raw_or_empty_object(writer, arguments);
    (void)writer.end_object();
    if (!writer.finish()) {
        return std::string{};
    }
    return std::move(out);
}

std::expected<ToolCallParams, McpError> ToolCallParams::from_json(const json::Json& element) {
    auto objExp = require_object(element, "tool call params");
    if (!objExp) {
        return std::unexpected(objExp.error());
    }
    json::Json obj = objExp.value();

    ToolCallParams p;
    auto nameExp = require_string(obj, "name");
    if (!nameExp) {
        return std::unexpected(nameExp.error());
    }
    p.name = nameExp.value();

    json::Json argsElement = obj.at("arguments");
    if (argsElement.valid()) {
        std::string raw;
        auto serialized = json::stream::serialize(argsElement, [&](std::string_view chunk) -> json::result<void> {
            raw.append(chunk);
            return {};
        });
        if (serialized) {
            p.arguments = std::move(raw);
        }
    }

    return p;
}

std::string ToolCallResult::to_json() const {
    std::string out;
    auto writer = make_json_writer(out);
    // StreamWriter 失败粘滞：中间结果统一丢弃，由 finish() 统一检查
    (void)writer.start_object();
    (void)writer.key("content");
    (void)writer.start_array();
    for (const auto& item : content) {
        (void)writer.raw(item.to_json());
    }
    (void)writer.end_array();
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

std::expected<ToolCallResult, McpError> ToolCallResult::from_json(const json::Json& element) {
    auto objExp = require_object(element, "tool call result");
    if (!objExp) {
        return std::unexpected(objExp.error());
    }
    json::Json obj = objExp.value();

    ToolCallResult r;

    json::Json contentArray = obj.at("content");
    if (contentArray.is_array()) {
        for (size_t i = 0; i < contentArray.size(); ++i) {
            const json::Json item = contentArray.at(i);
            auto contentExp = Content::from_json(item);
            if (!contentExp) {
                return std::unexpected(contentExp.error());
            }
            r.content.push_back(std::move(contentExp.value()));
        }
    }

    auto isErrorVal = obj.at("isError").as_bool();
    if (isErrorVal) {
        r.isError = *isErrorVal;
    }

    return r;
}

std::string JsonRpcRequest::to_json() const {
    std::string out;
    auto writer = make_json_writer(out);
    // StreamWriter 失败粘滞：中间结果统一丢弃，由 finish() 统一检查
    (void)writer.start_object();
    (void)writer.key("jsonrpc");
    (void)writer.string(jsonrpc);
    if (id.has_value()) {
        (void)writer.key("id");
        (void)writer.number(id.value());
    }
    (void)writer.key("method");
    (void)writer.string(method);
    if (params.has_value()) {
        (void)writer.key("params");
        write_raw_or_empty_object(writer, params.value());
    }
    (void)writer.end_object();
    if (!writer.finish()) {
        return std::string{};
    }
    return std::move(out);
}

std::string JsonRpcResponse::to_json() const {
    std::string out;
    auto writer = make_json_writer(out);
    // StreamWriter 失败粘滞：中间结果统一丢弃，由 finish() 统一检查
    (void)writer.start_object();
    (void)writer.key("jsonrpc");
    (void)writer.string(jsonrpc);
    (void)writer.key("id");
    (void)writer.number(id);
    if (result.has_value()) {
        (void)writer.key("result");
        write_raw_or_empty_object(writer, result.value());
    }
    if (error.has_value()) {
        (void)writer.key("error");
        write_raw_or_empty_object(writer, error.value());
    }
    (void)writer.end_object();
    if (!writer.finish()) {
        return std::string{};
    }
    return std::move(out);
}

std::expected<JsonRpcResponse, McpError> JsonRpcResponse::from_json(const json::Json& element) {
    auto objExp = require_object(element, "jsonrpc response");
    if (!objExp) {
        return std::unexpected(objExp.error());
    }
    json::Json obj = objExp.value();

    JsonRpcResponse r;
    auto idExp = require_int64(obj, "id");
    if (!idExp) {
        return std::unexpected(idExp.error());
    }
    r.id = idExp.value();

    json::Json resultElement = obj.at("result");
    if (resultElement.valid()) {
        std::string raw;
        auto serialized = json::stream::serialize(resultElement, [&](std::string_view chunk) -> json::result<void> {
            raw.append(chunk);
            return {};
        });
        if (serialized) {
            r.result = std::move(raw);
        }
    }

    json::Json errorElement = obj.at("error");
    if (errorElement.valid()) {
        std::string raw;
        auto serialized = json::stream::serialize(errorElement, [&](std::string_view chunk) -> json::result<void> {
            raw.append(chunk);
            return {};
        });
        if (serialized) {
            r.error = std::move(raw);
        }
    }

    return r;
}

std::string JsonRpcNotification::to_json() const {
    std::string out;
    auto writer = make_json_writer(out);
    // StreamWriter 失败粘滞：中间结果统一丢弃，由 finish() 统一检查
    (void)writer.start_object();
    (void)writer.key("jsonrpc");
    (void)writer.string(jsonrpc);
    (void)writer.key("method");
    (void)writer.string(method);
    if (params.has_value()) {
        (void)writer.key("params");
        write_raw_or_empty_object(writer, params.value());
    }
    (void)writer.end_object();
    if (!writer.finish()) {
        return std::string{};
    }
    return std::move(out);
}

std::string JsonRpcError::to_json() const {
    std::string out;
    auto writer = make_json_writer(out);
    // StreamWriter 失败粘滞：中间结果统一丢弃，由 finish() 统一检查
    (void)writer.start_object();
    (void)writer.key("code");
    (void)writer.number(static_cast<int64_t>(code));
    (void)writer.key("message");
    (void)writer.string(message);
    if (data.has_value()) {
        (void)writer.key("data");
        write_raw_or_empty_object(writer, data.value());
    }
    (void)writer.end_object();
    if (!writer.finish()) {
        return std::string{};
    }
    return std::move(out);
}

std::expected<JsonRpcError, McpError> JsonRpcError::from_json(const json::Json& element) {
    auto objExp = require_object(element, "jsonrpc error");
    if (!objExp) {
        return std::unexpected(objExp.error());
    }
    json::Json obj = objExp.value();

    JsonRpcError e;
    auto codeExp = require_int64(obj, "code");
    if (!codeExp) {
        return std::unexpected(codeExp.error());
    }
    e.code = static_cast<int>(codeExp.value());
    auto msgExp = require_string(obj, "message");
    if (!msgExp) {
        return std::unexpected(msgExp.error());
    }
    e.message = msgExp.value();

    json::Json dataElement = obj.at("data");
    if (dataElement.valid()) {
        std::string raw;
        auto serialized = json::stream::serialize(dataElement, [&](std::string_view chunk) -> json::result<void> {
            raw.append(chunk);
            return {};
        });
        if (serialized) {
            e.data = std::move(raw);
        }
    }

    return e;
}

} // namespace galay::mcp
