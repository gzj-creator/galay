#include <galay/cpp/galay-mcp/server/http_server.h>
#include <galay/cpp/galay-mcp/common/mcp_log.h>
#include <galay/cpp/galay-mcp/common/protocol_utils.h>

namespace galay {
namespace mcp {

namespace {

JsonString EmptyObjectString() {
    return "{}";
}

JsonString MakeResultResponse(int64_t id, std::string_view resultJson) {
    const std::string idString = std::to_string(id);
    JsonString response;
    response.reserve(32 + idString.size() + resultJson.size());
    response += "{\"jsonrpc\":\"2.0\",\"id\":";
    response += idString;
    response += ",\"result\":";
    if (resultJson.empty()) {
        response += "{}";
    } else {
        response.append(resultJson.data(), resultJson.size());
    }
    response.push_back('}');
    return response;
}

} // namespace

McpHttpServer::McpHttpServer(const std::string& host,
                             int port,
                             size_t ioSchedulers,
                             size_t computeSchedulers)
    : m_host(host)
    , m_port(port)
    , m_serverName("galay-mcp-http-server")
    , m_serverVersion("1.0.0")
    , m_ioSchedulers(ioSchedulers)
    , m_computeSchedulers(computeSchedulers)
    , m_toolsCacheDirty(false)
    , m_resourcesCacheDirty(false)
    , m_promptsCacheDirty(false)
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

McpHttpServer::~McpHttpServer() {
    stop();
}

void McpHttpServer::setServerInfo(const std::string& name, const std::string& version) {
    m_serverName = name;
    m_serverVersion = version;
}

void McpHttpServer::setProductionPolicy(McpProductionPolicy policy) {
    m_policy = std::move(policy);
}

void McpHttpServer::addTool(const std::string& name,
                             const std::string& description,
                             const JsonString& inputSchema,
                             McpHttpServer::ToolHandler handler) {
    Tool tool;
    tool.name = name;
    tool.description = description;
    tool.inputSchema = inputSchema;

    ToolInfo info;
    info.tool = tool;
    info.handler = handler;

    m_tools[name] = info;
    m_toolsListCache = protocol::buildListResultFromMap(
        m_tools, "tools",
        [](const ToolInfo& info) -> const Tool& { return info.tool; });
    m_toolsCacheDirty = false;
}

void McpHttpServer::addResource(const std::string& uri,
                                 const std::string& name,
                                 const std::string& description,
                                 const std::string& mimeType,
                                 McpHttpServer::ResourceReader reader) {
    Resource resource;
    resource.uri = uri;
    resource.name = name;
    resource.description = description;
    resource.mimeType = mimeType;

    ResourceInfo info;
    info.resource = resource;
    info.reader = reader;

    m_resources[uri] = info;
    m_resourcesListCache = protocol::buildListResultFromMap(
        m_resources, "resources",
        [](const ResourceInfo& info) -> const Resource& { return info.resource; });
    m_resourcesCacheDirty = false;
}

void McpHttpServer::addPrompt(const std::string& name,
                               const std::string& description,
                               const std::vector<PromptArgument>& arguments,
                               McpHttpServer::PromptGetter getter) {
    Prompt prompt;
    prompt.name = name;
    prompt.description = description;
    prompt.arguments = arguments;

    PromptInfo info;
    info.prompt = prompt;
    info.getter = getter;

    m_prompts[name] = info;
    m_promptsListCache = protocol::buildListResultFromMap(
        m_prompts, "prompts",
        [](const PromptInfo& info) -> const Prompt& { return info.prompt; });
    m_promptsCacheDirty = false;
}

void McpHttpServer::start() {
    if (m_running) {
        return;
    }

    MCP_LOG_INFO("[http_server]", "starting host={} port={} io_schedulers={} compute_schedulers={}",
                 m_host,
                 m_port,
                 m_ioSchedulers,
                 m_computeSchedulers);
    m_router = std::make_unique<http::HttpRouter>();

    auto* serverPtr = this;
    m_router->addHandler<http::HttpMethod::POST>("/mcp",
        [serverPtr](http::HttpConn& conn, http::HttpRequest req) -> galay::kernel::Task<void> {
            // 每个连接独立保存初始化状态，禁止全局 initialized 授权其它连接。
            bool connectionInitialized = false;
            std::size_t keepAliveRequests = 0;

            // 处理第一个请求
            {
                const std::string& requestBody = req.bodyStr();
                JsonString responseJson;
                ++keepAliveRequests;
                if (keepAliveRequests > serverPtr->m_policy.transport.max_keep_alive_requests) {
                    responseJson = serverPtr->createErrorResponse(0, ErrorCodes::INVALID_REQUEST,
                                                                  "Keep-alive request limit exceeded", "");
                } else if (requestBody.size() > serverPtr->m_policy.transport.max_http_body_bytes) {
                    responseJson = serverPtr->createErrorResponse(0, ErrorCodes::INVALID_REQUEST,
                                                                  "Payload too large", "");
                } else {
                    try {
                        co_await serverPtr->processRequest(requestBody, responseJson, connectionInitialized);
                    } catch (const std::exception& e) {
                        MCP_LOG_WARN("[http_server]", "initial request failed error={}", e.what());
                        responseJson = serverPtr->createErrorResponse(0, ErrorCodes::INTERNAL_ERROR,
                                                                      "Internal error", "");
                    }
                }
                if (responseJson.size() > serverPtr->m_policy.transport.max_response_bytes) {
                    responseJson = serverPtr->createErrorResponse(0, ErrorCodes::INVALID_REQUEST,
                                                                  "Payload too large", "");
                }
                co_await serverPtr->sendJsonResponse(conn, responseJson);
            }

            // Keep-Alive: 循环处理后续请求，直到连接关闭
            auto reader = conn.getReader();
            while (true) {
                http::HttpRequest nextReq;
                while (true) {
                    auto result = co_await reader.getRequest(nextReq);
                    if (!result) {
                        MCP_LOG_DEBUG("[http_server]", "connection closed or read failed error={}",
                                      result.error().message());
                        // 连接关闭或出错
                        co_await conn.close();
                        co_return;
                    }
                    if (result.value()) {
                        // 请求完整
                        break;
                    }
                    // 请求不完整，继续读取
                }

                const std::string& requestBody = nextReq.bodyStr();

                JsonString responseJson;
                ++keepAliveRequests;
                if (keepAliveRequests > serverPtr->m_policy.transport.max_keep_alive_requests) {
                    responseJson = serverPtr->createErrorResponse(0, ErrorCodes::INVALID_REQUEST,
                                                                  "Keep-alive request limit exceeded", "");
                } else if (requestBody.size() > serverPtr->m_policy.transport.max_http_body_bytes) {
                    responseJson = serverPtr->createErrorResponse(0, ErrorCodes::INVALID_REQUEST,
                                                                  "Payload too large", "");
                } else {
                    try {
                        co_await serverPtr->processRequest(requestBody, responseJson, connectionInitialized);
                    } catch (const std::exception& e) {
                        MCP_LOG_WARN("[http_server]", "keepalive request failed error={}", e.what());
                        responseJson = serverPtr->createErrorResponse(0, ErrorCodes::INTERNAL_ERROR,
                                                                      "Internal error", "");
                    }
                }
                if (responseJson.size() > serverPtr->m_policy.transport.max_response_bytes) {
                    responseJson = serverPtr->createErrorResponse(0, ErrorCodes::INVALID_REQUEST,
                                                                  "Payload too large", "");
                }
                co_await serverPtr->sendJsonResponse(conn, responseJson);
            }
        });

    http::HttpServerConfig config;
    config.host = m_host;
    config.port = static_cast<uint16_t>(m_port);
    config.backlog = 128;
    config.io_scheduler_count = m_ioSchedulers;
    config.compute_scheduler_count = m_computeSchedulers;

    m_httpServer = std::make_unique<http::HttpServer>(config);
    m_running = true;
    m_httpServer->start(std::move(*m_router));

    while (m_running) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

void McpHttpServer::stop() {
    if (m_running) {
        MCP_LOG_INFO("[http_server]", "stopping host={} port={}", m_host, m_port);
    }
    m_running = false;
    m_initialized = false;
    if (m_httpServer) {
        m_httpServer->stop();
        m_httpServer.reset();
    }
    m_router.reset();
}

bool McpHttpServer::isRunning() const {
    return m_running;
}

galay::kernel::Task<void> McpHttpServer::sendJsonResponse(http::HttpConn& conn, const JsonString& responseJson) {
    JsonString wireBytes;
    const std::string serverHeader = m_serverName + "/" + m_serverVersion;
    const std::string contentLength = std::to_string(responseJson.size());
    wireBytes.reserve(serverHeader.size() + contentLength.size() + responseJson.size() + 96);
    wireBytes += "HTTP/1.1 200 OK\r\n";
    wireBytes += "Server: ";
    wireBytes += serverHeader;
    wireBytes += "\r\nContent-Type: application/json\r\nConnection: keep-alive\r\nContent-Length: ";
    wireBytes += contentLength;
    wireBytes += "\r\n\r\n";
    wireBytes += responseJson;

    auto writer = conn.getWriter();
    while (true) {
        auto send_result = co_await writer.send(std::move(wireBytes));
        if (!send_result || send_result.value()) {
            break;
        }
    }
    co_return;
}

galay::kernel::Task<void> McpHttpServer::processRequest(const std::string& requestBody, JsonString& responseJson, bool& connectionInitialized) {
    try {
        auto parsed = parseJsonRpcRequest(requestBody);
        if (!parsed) {
            MCP_LOG_WARN("[http_server]", "json-rpc request parse failed error={}",
                         parsed.error().details());
            responseJson = createErrorResponse(0,
                                   parsed.error().toJsonRpcErrorCode(),
                                   parsed.error().message(),
                                   parsed.error().details());
            co_return;
        }

        const JsonRpcRequestView& request = parsed.value().request;
        const std::string& method = request.method;

        if (method == Methods::INITIALIZE) {
            responseJson = handleInitialize(request, connectionInitialized);
        } else if (method == Methods::TOOLS_LIST) {
            responseJson = handleToolsList(request, connectionInitialized);
        } else if (method == Methods::TOOLS_CALL) {
            co_await handleToolsCall(request, responseJson, connectionInitialized);
        } else if (method == Methods::RESOURCES_LIST) {
            responseJson = handleResourcesList(request, connectionInitialized);
        } else if (method == Methods::RESOURCES_READ) {
            co_await handleResourcesRead(request, responseJson, connectionInitialized);
        } else if (method == Methods::PROMPTS_LIST) {
            responseJson = handlePromptsList(request, connectionInitialized);
        } else if (method == Methods::PROMPTS_GET) {
            co_await handlePromptsGet(request, responseJson, connectionInitialized);
        } else if (method == Methods::PING) {
            responseJson = handlePing(request);
        } else {
            if (request.id.has_value()) {
                MCP_LOG_WARN("[http_server]", "method not found method={} id={}", method, request.id.value());
                responseJson = createErrorResponse(request.id.value(),
                                         ErrorCodes::METHOD_NOT_FOUND,
                                         "Method not found", method);
            } else {
                responseJson = EmptyObjectString();
            }
        }
    } catch (const std::exception& e) {
        MCP_LOG_WARN("[http_server]", "process request failed error={}", e.what());
        responseJson = createErrorResponse(0, ErrorCodes::INVALID_REQUEST,
                                  "Invalid request", "");
    }
    co_return;
}

JsonString McpHttpServer::handleInitialize(const JsonRpcRequestView& request, bool& connectionInitialized) {
    if (!request.id.has_value()) {
        return EmptyObjectString();
    }

    if (connectionInitialized) {
        return createErrorResponse(request.id.value(), ErrorCodes::INVALID_REQUEST,
                                  "Already initialized", "");
    }

    if (!request.hasParams) {
        MCP_LOG_WARN("[http_server]", "initialize missing params id={}", request.id.value());
        return createErrorResponse(request.id.value(), ErrorCodes::INVALID_PARAMS,
                                  "Invalid parameters", "Missing params");
    }

    auto paramsExp = InitializeParams::fromJson(request.params);
    if (!paramsExp) {
        MCP_LOG_WARN("[http_server]", "initialize params parse failed id={} error={}",
                     request.id.value(),
                     paramsExp.error().message());
        return createErrorResponse(request.id.value(), ErrorCodes::INVALID_PARAMS,
                                  "Invalid parameters", paramsExp.error().message());
    }

    JsonString result = protocol::buildInitializeResult(
        m_serverName,
        m_serverVersion,
        !m_tools.empty(),
        !m_resources.empty(),
        !m_prompts.empty());

    connectionInitialized = true;
    MCP_LOG_INFO("[http_server]", "initialized id={} server={}/{}",
                 request.id.value(),
                 m_serverName,
                 m_serverVersion);

    return MakeResultResponse(request.id.value(), result);
}

JsonString McpHttpServer::handleToolsList(const JsonRpcRequestView& request, bool& connectionInitialized) {
    if (!request.id.has_value()) {
        return EmptyObjectString();
    }

    if (!connectionInitialized) {
        MCP_LOG_WARN("[http_server]", "tools/list before initialization id={}", request.id.value());
        return createErrorResponse(request.id.value(), ErrorCodes::INVALID_REQUEST,
                                  "Not initialized", "");
    }

    return MakeResultResponse(request.id.value(), getToolsListResult());
}

galay::kernel::Task<void> McpHttpServer::handleToolsCall(const JsonRpcRequestView& request, JsonString& responseJson, bool& connectionInitialized) {
    if (!request.id.has_value()) {
        responseJson = EmptyObjectString();
        co_return;
    }

    if (!connectionInitialized) {
        MCP_LOG_WARN("[http_server]", "tools/call before initialization id={}", request.id.value());
        responseJson = createErrorResponse(request.id.value(), ErrorCodes::INVALID_REQUEST,
                                  "Not initialized", "");
        co_return;
    }

    try {
        if (!request.hasParams) {
            MCP_LOG_WARN("[http_server]", "tools/call missing params id={}", request.id.value());
            responseJson = createErrorResponse(request.id.value(), ErrorCodes::INVALID_PARAMS,
                                      "Invalid parameters", "Missing params");
            co_return;
        }

        JsonObject paramsObj;
        if (!JsonHelper::GetObject(request.params, paramsObj)) {
            MCP_LOG_WARN("[http_server]", "tools/call params not object id={}", request.id.value());
            responseJson = createErrorResponse(request.id.value(), ErrorCodes::INVALID_PARAMS,
                                      "Invalid parameters", "Params must be object");
            co_return;
        }

        std::string toolName;
        if (!JsonHelper::GetString(paramsObj, "name", toolName)) {
            MCP_LOG_WARN("[http_server]", "tools/call missing tool name id={}", request.id.value());
            responseJson = createErrorResponse(request.id.value(), ErrorCodes::INVALID_PARAMS,
                                      "Invalid parameters", "Missing tool name");
            co_return;
        }

        auto it = m_tools.find(toolName);
        if (it == m_tools.end()) {
            MCP_LOG_WARN("[http_server]", "tool not found id={} tool={}", request.id.value(), toolName);
            responseJson = createErrorResponse(request.id.value(), ErrorCodes::METHOD_NOT_FOUND,
                                      "Tool not found", toolName);
            co_return;
        }

        McpHttpServer::ToolHandler handler = it->second.handler;

        JsonElement arguments = JsonHelper::EmptyObject();
        JsonElement argsElement;
        if (JsonHelper::GetElement(paramsObj, "arguments", argsElement)) {
            arguments = argsElement;
        }

        // 调用工具处理函数（协程）
        std::expected<JsonString, McpError> result;
        co_await handler(arguments, result);

        if (!result) {
            MCP_LOG_WARN("[http_server]", "tool handler failed id={} tool={} error={}",
                         request.id.value(),
                         toolName,
                         result.error().message());
            responseJson = createErrorResponse(request.id.value(),
                                      result.error().toJsonRpcErrorCode(),
                                      result.error().message(),
                                      result.error().details());
            co_return;
        }

        ToolCallResult callResult;
        Content content;
        content.type = ContentType::Text;
        content.text = result.value();
        callResult.content.push_back(content);

        responseJson = MakeResultResponse(request.id.value(), callResult.toJson());

    } catch (const std::exception& e) {
        MCP_LOG_ERROR("[http_server]", "tools/call threw id={} error={}", request.id.value(), e.what());
        responseJson = createErrorResponse(request.id.value(), ErrorCodes::INTERNAL_ERROR,
                                  "Internal error", "");
    }
    co_return;
}

JsonString McpHttpServer::handleResourcesList(const JsonRpcRequestView& request, bool& connectionInitialized) {
    if (!request.id.has_value()) {
        return EmptyObjectString();
    }

    if (!connectionInitialized) {
        MCP_LOG_WARN("[http_server]", "resources/list before initialization id={}", request.id.value());
        return createErrorResponse(request.id.value(), ErrorCodes::INVALID_REQUEST,
                                  "Not initialized", "");
    }

    return MakeResultResponse(request.id.value(), getResourcesListResult());
}

galay::kernel::Task<void> McpHttpServer::handleResourcesRead(const JsonRpcRequestView& request, JsonString& responseJson, bool& connectionInitialized) {
    if (!request.id.has_value()) {
        responseJson = EmptyObjectString();
        co_return;
    }

    if (!connectionInitialized) {
        MCP_LOG_WARN("[http_server]", "resources/read before initialization id={}", request.id.value());
        responseJson = createErrorResponse(request.id.value(), ErrorCodes::INVALID_REQUEST,
                                  "Not initialized", "");
        co_return;
    }

    try {
        if (!request.hasParams) {
            MCP_LOG_WARN("[http_server]", "resources/read missing params id={}", request.id.value());
            responseJson = createErrorResponse(request.id.value(), ErrorCodes::INVALID_PARAMS,
                                      "Invalid parameters", "Missing params");
            co_return;
        }

        JsonObject paramsObj;
        if (!JsonHelper::GetObject(request.params, paramsObj)) {
            MCP_LOG_WARN("[http_server]", "resources/read params not object id={}", request.id.value());
            responseJson = createErrorResponse(request.id.value(), ErrorCodes::INVALID_PARAMS,
                                      "Invalid parameters", "Params must be object");
            co_return;
        }

        std::string uri;
        if (!JsonHelper::GetString(paramsObj, "uri", uri)) {
            MCP_LOG_WARN("[http_server]", "resources/read missing uri id={}", request.id.value());
            responseJson = createErrorResponse(request.id.value(), ErrorCodes::INVALID_PARAMS,
                                      "Invalid parameters", "Missing uri");
            co_return;
        }

        auto it = m_resources.find(uri);
        if (it == m_resources.end()) {
            MCP_LOG_WARN("[http_server]", "resource not found id={} uri={}", request.id.value(), uri);
            responseJson = createErrorResponse(request.id.value(), ErrorCodes::METHOD_NOT_FOUND,
                                      "Resource not found", uri);
            co_return;
        }

        McpHttpServer::ResourceReader reader = it->second.reader;

        // 调用资源读取函数（协程）
        std::expected<std::string, McpError> result;
        co_await reader(uri, result);

        if (!result) {
            MCP_LOG_WARN("[http_server]", "resource reader failed id={} uri={} error={}",
                         request.id.value(),
                         uri,
                         result.error().message());
            responseJson = createErrorResponse(request.id.value(),
                                      result.error().toJsonRpcErrorCode(),
                                      result.error().message(),
                                      result.error().details());
            co_return;
        }

        Content content;
        content.type = ContentType::Text;
        content.text = result.value();

        JsonWriter resultWriter;
        resultWriter.StartObject();
        resultWriter.Key("contents");
        resultWriter.StartArray();
        resultWriter.Raw(content.toJson());
        resultWriter.EndArray();
        resultWriter.EndObject();

        responseJson = MakeResultResponse(request.id.value(), resultWriter.TakeString());

    } catch (const std::exception& e) {
        MCP_LOG_ERROR("[http_server]", "resources/read threw id={} error={}", request.id.value(), e.what());
        responseJson = createErrorResponse(request.id.value(), ErrorCodes::INTERNAL_ERROR,
                                  "Internal error", "");
    }
    co_return;
}

JsonString McpHttpServer::handlePromptsList(const JsonRpcRequestView& request, bool& connectionInitialized) {
    if (!request.id.has_value()) {
        return EmptyObjectString();
    }

    if (!connectionInitialized) {
        MCP_LOG_WARN("[http_server]", "prompts/list before initialization id={}", request.id.value());
        return createErrorResponse(request.id.value(), ErrorCodes::INVALID_REQUEST,
                                  "Not initialized", "");
    }

    return MakeResultResponse(request.id.value(), getPromptsListResult());
}

galay::kernel::Task<void> McpHttpServer::handlePromptsGet(const JsonRpcRequestView& request, JsonString& responseJson, bool& connectionInitialized) {
    if (!request.id.has_value()) {
        responseJson = EmptyObjectString();
        co_return;
    }

    if (!connectionInitialized) {
        MCP_LOG_WARN("[http_server]", "prompts/get before initialization id={}", request.id.value());
        responseJson = createErrorResponse(request.id.value(), ErrorCodes::INVALID_REQUEST,
                                  "Not initialized", "");
        co_return;
    }

    try {
        if (!request.hasParams) {
            MCP_LOG_WARN("[http_server]", "prompts/get missing params id={}", request.id.value());
            responseJson = createErrorResponse(request.id.value(), ErrorCodes::INVALID_PARAMS,
                                      "Invalid parameters", "Missing params");
            co_return;
        }

        JsonObject paramsObj;
        if (!JsonHelper::GetObject(request.params, paramsObj)) {
            MCP_LOG_WARN("[http_server]", "prompts/get params not object id={}", request.id.value());
            responseJson = createErrorResponse(request.id.value(), ErrorCodes::INVALID_PARAMS,
                                      "Invalid parameters", "Params must be object");
            co_return;
        }

        std::string name;
        if (!JsonHelper::GetString(paramsObj, "name", name)) {
            MCP_LOG_WARN("[http_server]", "prompts/get missing name id={}", request.id.value());
            responseJson = createErrorResponse(request.id.value(), ErrorCodes::INVALID_PARAMS,
                                      "Invalid parameters", "Missing prompt name");
            co_return;
        }

        JsonElement arguments = JsonHelper::EmptyObject();
        JsonElement argsElement;
        if (JsonHelper::GetElement(paramsObj, "arguments", argsElement)) {
            arguments = argsElement;
        }

        auto it = m_prompts.find(name);
        if (it == m_prompts.end()) {
            MCP_LOG_WARN("[http_server]", "prompt not found id={} name={}", request.id.value(), name);
            responseJson = createErrorResponse(request.id.value(), ErrorCodes::METHOD_NOT_FOUND,
                                      "Prompt not found", name);
            co_return;
        }

        McpHttpServer::PromptGetter getter = it->second.getter;

        // 调用提示获取函数（协程）
        std::expected<JsonString, McpError> result;
        co_await getter(name, arguments, result);

        if (!result) {
            MCP_LOG_WARN("[http_server]", "prompt getter failed id={} name={} error={}",
                         request.id.value(),
                         name,
                         result.error().message());
            responseJson = createErrorResponse(request.id.value(),
                                      result.error().toJsonRpcErrorCode(),
                                      result.error().message(),
                                      result.error().details());
            co_return;
        }

        responseJson = MakeResultResponse(request.id.value(), result.value());

    } catch (const std::exception& e) {
        MCP_LOG_ERROR("[http_server]", "prompts/get threw id={} error={}", request.id.value(), e.what());
        responseJson = createErrorResponse(request.id.value(), ErrorCodes::INTERNAL_ERROR,
                                  "Internal error", "");
    }
    co_return;
}

JsonString McpHttpServer::handlePing(const JsonRpcRequestView& request) {
    if (!request.id.has_value()) {
        return EmptyObjectString();
    }

    return MakeResultResponse(request.id.value(), EmptyObjectString());
}

JsonString McpHttpServer::createErrorResponse(int64_t id, int code,
                                        const std::string& message,
                                        const std::string& details) {
    return protocol::makeErrorResponse(id, code, message, details).toJson();
}

const JsonString& McpHttpServer::getToolsListResult() {
    return m_toolsListCache;
}

const JsonString& McpHttpServer::getResourcesListResult() {
    return m_resourcesListCache;
}

const JsonString& McpHttpServer::getPromptsListResult() {
    return m_promptsListCache;
}

} // namespace mcp
} // namespace galay
