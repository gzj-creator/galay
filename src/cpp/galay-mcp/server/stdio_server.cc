#include <galay/cpp/galay-mcp/server/stdio_server.h>
#include <galay/cpp/galay-mcp/common/mcp_log.h>
#include <galay/cpp/galay-mcp/common/protocol_utils.h>
#include <sstream>
#include <stdexcept>
#include <mutex>

namespace galay {
namespace mcp {

namespace {

JsonString emptyObjectString() {
    return "{}";
}

} // namespace

McpStdioServer::McpStdioServer()
    : m_serverName("galay-mcp-server")
    , m_serverVersion("1.0.0")
    , m_input(&std::cin)
    , m_output(&std::cout)
    , m_running(false)
    , m_initialized(false) {
    m_toolsListCache = protocol::buildListResultFromMap(
        m_tools, "tools",
        [](const ToolInfo& info) -> const Tool& { return info.tool; });
    m_resourcesListCache = protocol::buildListResultFromMap(
        m_resources, "resources",
        [](const ResourceInfo& info) -> const Resource& { return info.resource; });
    m_promptsListCache = protocol::buildListResultFromMap(
        m_prompts, "prompts",
        [](const PromptInfo& info) -> const Prompt& { return info.prompt; });
}

McpStdioServer::~McpStdioServer() {
    stop();
}

void McpStdioServer::setServerInfo(const std::string& name, const std::string& version) {
    m_serverName = name;
    m_serverVersion = version;
}

void McpStdioServer::setProductionPolicy(McpProductionPolicy policy) {
    m_policy = std::move(policy);
}

void McpStdioServer::setStreams(std::istream& input, std::ostream& output) noexcept {
    m_input = &input;
    m_output = &output;
}

void McpStdioServer::addTool(std::string name,
                             std::string description,
                             JsonString inputSchema,
                             McpStdioServer::ToolHandler handler) {
    std::unique_lock<std::shared_mutex> lock(m_toolsMutex);

    Tool tool;
    tool.name = std::move(name);
    tool.description = std::move(description);
    tool.inputSchema = std::move(inputSchema);

    ToolInfo info;
    info.tool = std::move(tool);
    info.handler = std::move(handler);

    std::string key = info.tool.name;
    m_tools.insert_or_assign(std::move(key), std::move(info));
    m_toolsListCache = protocol::buildListResultFromMap(
        m_tools, "tools",
        [](const ToolInfo& info) -> const Tool& { return info.tool; });
}

void McpStdioServer::addResource(std::string uri,
                                 std::string name,
                                 std::string description,
                                 std::string mimeType,
                                 McpStdioServer::ResourceReader reader) {
    std::unique_lock<std::shared_mutex> lock(m_resourcesMutex);

    Resource resource;
    resource.uri = std::move(uri);
    resource.name = std::move(name);
    resource.description = std::move(description);
    resource.mimeType = std::move(mimeType);

    ResourceInfo info;
    info.resource = std::move(resource);
    info.reader = std::move(reader);

    std::string key = info.resource.uri;
    m_resources.insert_or_assign(std::move(key), std::move(info));
    m_resourcesListCache = protocol::buildListResultFromMap(
        m_resources, "resources",
        [](const ResourceInfo& info) -> const Resource& { return info.resource; });
}

void McpStdioServer::addPrompt(std::string name,
                               std::string description,
                               std::vector<PromptArgument> arguments,
                               McpStdioServer::PromptGetter getter) {
    std::unique_lock<std::shared_mutex> lock(m_promptsMutex);

    Prompt prompt;
    prompt.name = std::move(name);
    prompt.description = std::move(description);
    prompt.arguments = std::move(arguments);

    PromptInfo info;
    info.prompt = std::move(prompt);
    info.getter = std::move(getter);

    std::string key = info.prompt.name;
    m_prompts.insert_or_assign(std::move(key), std::move(info));
    m_promptsListCache = protocol::buildListResultFromMap(
        m_prompts, "prompts",
        [](const PromptInfo& info) -> const Prompt& { return info.prompt; });
}

void McpStdioServer::run() {
    m_running = true;
    MCP_LOG_INFO("[stdio_server]", "run loop started server={}/{}", m_serverName, m_serverVersion);

    while (m_running) {
        auto messageResult = readMessage();
        if (!messageResult) {
            if (messageResult.error().code() == McpErrorCode::PayloadTooLarge) {
                sendError(0, messageResult.error().toJsonRpcErrorCode(), messageResult.error().message(), "");
                continue;
            }
            // 读取失败，可能是EOF或错误
            if (m_input->eof()) {
                MCP_LOG_INFO("[stdio_server]", "input reached eof");
                break;
            }
            MCP_LOG_WARN("[stdio_server]", "read message failed error={}", messageResult.error().message());
            continue;
        }

        auto parsed = parseJsonRpcRequest(messageResult.value());
        if (!parsed) {
            MCP_LOG_WARN("[stdio_server]", "json-rpc request parse failed error={}",
                         parsed.error().details());
            sendError(0, ErrorCodes::PARSE_ERROR, "Parse error", parsed.error().details());
            continue;
        }

        handleRequest(parsed.value().request);
    }

    m_running = false;
}

void McpStdioServer::stop() {
    if (m_running) {
        MCP_LOG_INFO("[stdio_server]", "run loop stopping");
    }
    m_running = false;
}

bool McpStdioServer::isRunning() const {
    return m_running;
}

void McpStdioServer::handleRequest(const JsonRpcRequestView& request) {
    const std::string& method = request.method;

    if (method == Methods::INITIALIZE) {
        handleInitialize(request);
    } else if (method == Methods::TOOLS_LIST) {
        handleToolsList(request);
    } else if (method == Methods::TOOLS_CALL) {
        handleToolsCall(request);
    } else if (method == Methods::RESOURCES_LIST) {
        handleResourcesList(request);
    } else if (method == Methods::RESOURCES_READ) {
        handleResourcesRead(request);
    } else if (method == Methods::PROMPTS_LIST) {
        handlePromptsList(request);
    } else if (method == Methods::PROMPTS_GET) {
        handlePromptsGet(request);
    } else if (method == Methods::PING) {
        handlePing(request);
    } else {
        if (request.id.has_value()) {
            MCP_LOG_WARN("[stdio_server]", "method not found method={} id={}", method, request.id.value());
            sendError(request.id.value(), ErrorCodes::METHOD_NOT_FOUND,
                     "Method not found", method);
        }
    }
}

void McpStdioServer::handleInitialize(const JsonRpcRequestView& request) {
    if (!request.id.has_value()) {
        return;
    }

    if (m_initialized) {
        MCP_LOG_WARN("[stdio_server]", "initialize rejected: already initialized id={}", request.id.value());
        sendError(request.id.value(), ErrorCodes::INVALID_REQUEST,
                 "Already initialized", "");
        return;
    }

    if (!request.hasParams) {
        MCP_LOG_WARN("[stdio_server]", "initialize missing params id={}", request.id.value());
        sendError(request.id.value(), ErrorCodes::INVALID_PARAMS,
                 "Invalid parameters", "Missing params");
        return;
    }

    auto paramsExp = InitializeParams::fromJson(request.params);
    if (!paramsExp) {
        MCP_LOG_WARN("[stdio_server]", "initialize params parse failed id={} error={}",
                     request.id.value(),
                     paramsExp.error().message());
        sendError(request.id.value(), ErrorCodes::INVALID_PARAMS,
                 "Invalid parameters", paramsExp.error().message());
        return;
    }

    // 构建响应
    JsonString result = protocol::buildInitializeResult(
        m_serverName,
        m_serverVersion,
        !m_tools.empty(),
        !m_resources.empty(),
        !m_prompts.empty());

    JsonRpcResponse response = protocol::makeResultResponse(request.id.value(), result);

    sendResponse(response);

    m_initialized = true;
    MCP_LOG_INFO("[stdio_server]", "initialized id={} server={}/{}",
                 request.id.value(),
                 m_serverName,
                 m_serverVersion);

    // 发送initialized通知
    sendNotification(Methods::INITIALIZED, emptyObjectString());
}

void McpStdioServer::handleToolsList(const JsonRpcRequestView& request) {
    if (!request.id.has_value()) {
        return;
    }

    if (!m_initialized) {
        MCP_LOG_WARN("[stdio_server]", "tools/list before initialization id={}", request.id.value());
        sendError(request.id.value(), ErrorCodes::INVALID_REQUEST,
                 "Not initialized", "");
        return;
    }

    std::shared_lock<std::shared_mutex> lock(m_toolsMutex);

    JsonRpcResponse response = protocol::makeResultResponse(
        request.id.value(), m_toolsListCache);

    sendResponse(response);
}

void McpStdioServer::handleToolsCall(const JsonRpcRequestView& request) {
    if (!request.id.has_value()) {
        return;
    }

    if (!m_initialized) {
        MCP_LOG_WARN("[stdio_server]", "tools/call before initialization id={}", request.id.value());
        sendError(request.id.value(), ErrorCodes::INVALID_REQUEST,
                 "Not initialized", "");
        return;
    }

    try {
        if (!request.hasParams) {
            MCP_LOG_WARN("[stdio_server]", "tools/call missing params id={}", request.id.value());
            sendError(request.id.value(), ErrorCodes::INVALID_PARAMS,
                     "Invalid parameters", "Missing params");
            return;
        }

        JsonObject paramsObj;
        if (!JsonHelper::getObject(request.params, paramsObj)) {
            MCP_LOG_WARN("[stdio_server]", "tools/call params not object id={}", request.id.value());
            sendError(request.id.value(), ErrorCodes::INVALID_PARAMS,
                     "Invalid parameters", "Params must be object");
            return;
        }

        std::string toolName;
        if (!JsonHelper::getString(paramsObj, "name", toolName)) {
            MCP_LOG_WARN("[stdio_server]", "tools/call missing tool name id={}", request.id.value());
            sendError(request.id.value(), ErrorCodes::INVALID_PARAMS,
                     "Invalid parameters", "Missing tool name");
            return;
        }

        std::shared_lock<std::shared_mutex> lock(m_toolsMutex);

        auto it = m_tools.find(toolName);
        if (it == m_tools.end()) {
            MCP_LOG_WARN("[stdio_server]", "tool not found id={} tool={}", request.id.value(), toolName);
            sendError(request.id.value(), ErrorCodes::METHOD_NOT_FOUND,
                     "Tool not found", toolName);
            return;
        }

        JsonElement arguments = JsonHelper::emptyObject();
        JsonElement argsElement;
        if (JsonHelper::getElement(paramsObj, "arguments", argsElement)) {
            arguments = argsElement;
        }

        // 调用工具处理函数
        auto result = it->second.handler(arguments);

        if (!result) {
            MCP_LOG_WARN("[stdio_server]", "tool handler failed id={} tool={} error={}",
                         request.id.value(),
                         toolName,
                         result.error().message());
            sendError(request.id.value(), result.error().toJsonRpcErrorCode(),
                     result.error().message(), result.error().details());
            return;
        }

        // 构建响应
        ToolCallResult callResult;
        Content content;
        content.type = ContentType::Text;
        content.text = result.value();
        callResult.content.push_back(content);

        JsonRpcResponse response;
        response.id = request.id.value();
        response.result = callResult.toJson();

        sendResponse(response);

    } catch (const std::exception& e) {
        MCP_LOG_ERROR("[stdio_server]", "tools/call threw id={} error={}", request.id.value(), e.what());
        sendError(request.id.value(), ErrorCodes::INTERNAL_ERROR,
                 "Internal error", "");
    }
}

void McpStdioServer::handleResourcesList(const JsonRpcRequestView& request) {
    if (!request.id.has_value()) {
        return;
    }

    if (!m_initialized) {
        MCP_LOG_WARN("[stdio_server]", "resources/list before initialization id={}", request.id.value());
        sendError(request.id.value(), ErrorCodes::INVALID_REQUEST,
                 "Not initialized", "");
        return;
    }

    std::shared_lock<std::shared_mutex> lock(m_resourcesMutex);

    JsonRpcResponse response = protocol::makeResultResponse(
        request.id.value(), m_resourcesListCache);

    sendResponse(response);
}

void McpStdioServer::handleResourcesRead(const JsonRpcRequestView& request) {
    if (!request.id.has_value()) {
        return;
    }

    if (!m_initialized) {
        MCP_LOG_WARN("[stdio_server]", "resources/read before initialization id={}", request.id.value());
        sendError(request.id.value(), ErrorCodes::INVALID_REQUEST,
                 "Not initialized", "");
        return;
    }

    try {
        if (!request.hasParams) {
            MCP_LOG_WARN("[stdio_server]", "resources/read missing params id={}", request.id.value());
            sendError(request.id.value(), ErrorCodes::INVALID_PARAMS,
                     "Invalid parameters", "Missing params");
            return;
        }

        JsonObject paramsObj;
        if (!JsonHelper::getObject(request.params, paramsObj)) {
            MCP_LOG_WARN("[stdio_server]", "resources/read params not object id={}", request.id.value());
            sendError(request.id.value(), ErrorCodes::INVALID_PARAMS,
                     "Invalid parameters", "Params must be object");
            return;
        }

        std::string uri;
        if (!JsonHelper::getString(paramsObj, "uri", uri)) {
            MCP_LOG_WARN("[stdio_server]", "resources/read missing uri id={}", request.id.value());
            sendError(request.id.value(), ErrorCodes::INVALID_PARAMS,
                     "Invalid parameters", "Missing uri");
            return;
        }

        std::shared_lock<std::shared_mutex> lock(m_resourcesMutex);

        auto it = m_resources.find(uri);
        if (it == m_resources.end()) {
            MCP_LOG_WARN("[stdio_server]", "resource not found id={} uri={}", request.id.value(), uri);
            sendError(request.id.value(), ErrorCodes::METHOD_NOT_FOUND,
                     "Resource not found", uri);
            return;
        }

        // 调用资源读取函数
        auto result = it->second.reader(uri);

        if (!result) {
            MCP_LOG_WARN("[stdio_server]", "resource reader failed id={} uri={} error={}",
                         request.id.value(),
                         uri,
                         result.error().message());
            sendError(request.id.value(), result.error().toJsonRpcErrorCode(),
                     result.error().message(), result.error().details());
            return;
        }

        // 构建响应
        Content content;
        content.type = ContentType::Text;
        content.text = result.value();

        JsonWriter resultWriter;
        resultWriter.startObject();
        resultWriter.key("contents");
        resultWriter.startArray();
        resultWriter.raw(content.toJson());
        resultWriter.endArray();
        resultWriter.endObject();

        JsonRpcResponse response;
        response.id = request.id.value();
        response.result = resultWriter.takeString();

        sendResponse(response);

    } catch (const std::exception& e) {
        MCP_LOG_ERROR("[stdio_server]", "resources/read threw id={} error={}", request.id.value(), e.what());
        sendError(request.id.value(), ErrorCodes::INTERNAL_ERROR,
                 "Internal error", "");
    }
}

void McpStdioServer::handlePromptsList(const JsonRpcRequestView& request) {
    if (!request.id.has_value()) {
        return;
    }

    if (!m_initialized) {
        MCP_LOG_WARN("[stdio_server]", "prompts/list before initialization id={}", request.id.value());
        sendError(request.id.value(), ErrorCodes::INVALID_REQUEST,
                 "Not initialized", "");
        return;
    }

    std::shared_lock<std::shared_mutex> lock(m_promptsMutex);

    JsonRpcResponse response = protocol::makeResultResponse(
        request.id.value(), m_promptsListCache);

    sendResponse(response);
}

void McpStdioServer::handlePromptsGet(const JsonRpcRequestView& request) {
    if (!request.id.has_value()) {
        return;
    }

    if (!m_initialized) {
        MCP_LOG_WARN("[stdio_server]", "prompts/get before initialization id={}", request.id.value());
        sendError(request.id.value(), ErrorCodes::INVALID_REQUEST,
                 "Not initialized", "");
        return;
    }

    try {
        if (!request.hasParams) {
            MCP_LOG_WARN("[stdio_server]", "prompts/get missing params id={}", request.id.value());
            sendError(request.id.value(), ErrorCodes::INVALID_PARAMS,
                     "Invalid parameters", "Missing params");
            return;
        }

        JsonObject paramsObj;
        if (!JsonHelper::getObject(request.params, paramsObj)) {
            MCP_LOG_WARN("[stdio_server]", "prompts/get params not object id={}", request.id.value());
            sendError(request.id.value(), ErrorCodes::INVALID_PARAMS,
                     "Invalid parameters", "Params must be object");
            return;
        }

        std::string name;
        if (!JsonHelper::getString(paramsObj, "name", name)) {
            MCP_LOG_WARN("[stdio_server]", "prompts/get missing name id={}", request.id.value());
            sendError(request.id.value(), ErrorCodes::INVALID_PARAMS,
                     "Invalid parameters", "Missing prompt name");
            return;
        }

        JsonElement arguments = JsonHelper::emptyObject();
        JsonElement argsElement;
        if (JsonHelper::getElement(paramsObj, "arguments", argsElement)) {
            arguments = argsElement;
        }

        std::shared_lock<std::shared_mutex> lock(m_promptsMutex);

        auto it = m_prompts.find(name);
        if (it == m_prompts.end()) {
            MCP_LOG_WARN("[stdio_server]", "prompt not found id={} name={}", request.id.value(), name);
            sendError(request.id.value(), ErrorCodes::METHOD_NOT_FOUND,
                     "Prompt not found", name);
            return;
        }

        // 调用提示获取函数
        auto result = it->second.getter(name, arguments);

        if (!result) {
            MCP_LOG_WARN("[stdio_server]", "prompt getter failed id={} name={} error={}",
                         request.id.value(),
                         name,
                         result.error().message());
            sendError(request.id.value(), result.error().toJsonRpcErrorCode(),
                     result.error().message(), result.error().details());
            return;
        }

        JsonRpcResponse response;
        response.id = request.id.value();
        response.result = result.value();

        sendResponse(response);

    } catch (const std::exception& e) {
        MCP_LOG_ERROR("[stdio_server]", "prompts/get threw id={} error={}", request.id.value(), e.what());
        sendError(request.id.value(), ErrorCodes::INTERNAL_ERROR,
                 "Internal error", "");
    }
}

void McpStdioServer::handlePing(const JsonRpcRequestView& request) {
    if (!request.id.has_value()) {
        return;
    }

    JsonRpcResponse response = protocol::makeResultResponse(
        request.id.value(), emptyObjectString());

    sendResponse(response);
}

void McpStdioServer::sendResponse(const JsonRpcResponse& response) {
    JsonString message = response.toJson();
    if (message.size() > m_policy.transport.max_response_bytes) {
        message = protocol::makeErrorResponse(
            response.id,
            ErrorCodes::INVALID_REQUEST,
            "Payload too large",
            "").toJson();
    }

    auto result = writeMessage(message);
    if (!result) {
        MCP_LOG_ERROR("[stdio_server]", "write response failed id={} error={}",
                      response.id,
                      result.error().message());
    }
}

void McpStdioServer::sendError(int64_t id, int code, const std::string& message,
                               const std::string& details) {
    sendResponse(protocol::makeErrorResponse(id, code, message, details));
}

void McpStdioServer::sendNotification(const std::string& method, const JsonString& params) {
    JsonRpcNotification notification;
    notification.method = method;
    notification.params = params;

    auto result = writeMessage(notification.toJson());
    if (!result) {
        MCP_LOG_ERROR("[stdio_server]", "write notification failed method={} error={}",
                      method,
                      result.error().message());
    }
}

std::expected<std::string, McpError> McpStdioServer::readMessage() {
    std::string line;
    if (!std::getline(*m_input, line)) {
        return std::unexpected(McpError::readError("Failed to read from stdin"));
    }

    if (line.empty()) {
        return std::unexpected(McpError::invalidMessage("Empty message"));
    }
    if (line.size() > m_policy.transport.max_stdio_line_bytes) {
        return std::unexpected(McpError::payloadTooLarge("stdio line exceeds configured limit"));
    }

    return line;
}

std::expected<void, McpError> McpStdioServer::writeMessage(const JsonString& message) {
    std::lock_guard<std::mutex> lock(m_outputMutex);

    try {
        *m_output << message << '\n';
        m_output->flush();
        return {};
    } catch (const std::exception& e) {
        return std::unexpected(McpError::writeError(e.what()));
    }
}

} // namespace mcp
} // namespace galay
