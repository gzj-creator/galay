#include "http_server.h"
#include "../../common/mcp_log.h"
#include "../../common/protocol_utils.h"

namespace galay::mcp {

namespace {

std::string empty_object_string() {
    return "{}";
}

std::string make_result_response(int64_t id, std::string_view resultJson) {
    std::string response;
    auto writer = make_json_writer(response);
    // StreamWriter 失败粘滞：中间结果统一丢弃，由 finish() 统一检查
    (void)writer.start_object();
    (void)writer.key("jsonrpc");
    (void)writer.string(JSONRPC_VERSION);
    (void)writer.key("id");
    (void)writer.number(id);
    (void)writer.key("result");
    if (resultJson.empty()) {
        (void)writer.start_object();
        (void)writer.end_object();
    } else {
        (void)writer.raw(resultJson);
    }
    (void)writer.end_object();
    if (!writer.finish()) {
        return std::string{};
    }
    return response;
}

} // namespace

McpHttpServer::McpHttpServer(const std::string& host,
                             int port,
                             size_t ioSchedulers,
                             size_t parallelSchedulers,
                             bool tcpNoDelay)
    : m_host(host)
    , m_serverName("galay-mcp-http-server")
    , m_serverVersion("1.0.0")
    , m_ioSchedulers(ioSchedulers)
    , m_parallelSchedulers(parallelSchedulers)
    , m_port(port)
    , m_tcpNoDelay(tcpNoDelay)
    , m_toolsCacheDirty(false)
    , m_resourcesCacheDirty(false)
    , m_promptsCacheDirty(false)
    , m_running(false)
    , m_initialized(false) {
    m_toolsListCache = protocol::build_list_result_from_map(
        m_tools, "tools",
        [](const ToolInfo& info) -> const Tool& { return info.tool; });
    m_resourcesListCache = protocol::build_list_result_from_map(
        m_resources, "resources",
        [](const ResourceInfo& info) -> const Resource& { return info.resource; });
    m_promptsListCache = protocol::build_list_result_from_map(
        m_prompts, "prompts",
        [](const PromptInfo& info) -> const Prompt& { return info.prompt; });
}

McpHttpServer::~McpHttpServer() {
    stop();
}

void McpHttpServer::set_server_info(const std::string& name, const std::string& version) {
    m_serverName = name;
    m_serverVersion = version;
}

void McpHttpServer::set_production_policy(McpProductionPolicy policy) {
    m_policy = std::move(policy);
}

void McpHttpServer::add_tool(std::string name,
                             std::string description,
                             std::string inputSchema,
                             McpHttpServer::ToolHandler handler) {
    Tool tool;
    tool.name = std::move(name);
    tool.description = std::move(description);
    tool.inputSchema = std::move(inputSchema);

    ToolInfo info;
    info.tool = std::move(tool);
    info.handler = std::move(handler);

    std::string key = info.tool.name;
    m_tools.insert_or_assign(std::move(key), std::move(info));
    m_toolsListCache = protocol::build_list_result_from_map(
        m_tools, "tools",
        [](const ToolInfo& info) -> const Tool& { return info.tool; });
    m_toolsCacheDirty = false;
}

void McpHttpServer::add_resource(std::string uri,
                                 std::string name,
                                 std::string description,
                                 std::string mimeType,
                                 McpHttpServer::ResourceReader reader) {
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
    m_resourcesListCache = protocol::build_list_result_from_map(
        m_resources, "resources",
        [](const ResourceInfo& info) -> const Resource& { return info.resource; });
    m_resourcesCacheDirty = false;
}

void McpHttpServer::add_prompt(std::string name,
                               std::string description,
                               std::vector<PromptArgument> arguments,
                               McpHttpServer::PromptGetter getter) {
    Prompt prompt;
    prompt.name = std::move(name);
    prompt.description = std::move(description);
    prompt.arguments = std::move(arguments);

    PromptInfo info;
    info.prompt = std::move(prompt);
    info.getter = std::move(getter);

    std::string key = info.prompt.name;
    m_prompts.insert_or_assign(std::move(key), std::move(info));
    m_promptsListCache = protocol::build_list_result_from_map(
        m_prompts, "prompts",
        [](const PromptInfo& info) -> const Prompt& { return info.prompt; });
    m_promptsCacheDirty = false;
}

void McpHttpServer::start() {
    if (m_running) {
        return;
    }

    MCP_LOG_INFO("[http_server]", "starting host={} port={} io_schedulers={} parallel_schedulers={}",
                 m_host,
                 m_port,
                 m_ioSchedulers,
                 m_parallelSchedulers);
    m_router = std::make_unique<http::HttpRouter>();

    auto* serverPtr = this;
    m_router->add_handler<http::HttpMethod::POST>("/mcp",
        [serverPtr](http::HttpConn& conn, http::HttpRequest req) -> galay::kernel::Task<void> {
            // 每个连接独立保存初始化状态，禁止全局 initialized 授权其它连接。
            bool connectionInitialized = false;
            std::size_t keepAliveRequests = 0;

            // 处理第一个请求
            {
                const std::string& requestBody = req.body_str();
                std::string responseJson;
                ++keepAliveRequests;
                if (keepAliveRequests > serverPtr->m_policy.transport.max_keep_alive_requests) {
                    responseJson = serverPtr->create_error_response(0, ErrorCodes::INVALID_REQUEST,
                                                                  "Keep-alive request limit exceeded", "");
                } else if (requestBody.size() > serverPtr->m_policy.transport.max_http_body_bytes) {
                    responseJson = serverPtr->create_error_response(0, ErrorCodes::INVALID_REQUEST,
                                                                  "Payload too large", "");
                } else {
                    try {
                        co_await serverPtr->process_request(requestBody, responseJson, connectionInitialized);
                    } catch (const std::exception& e) {
                        MCP_LOG_WARN("[http_server]", "initial request failed error={}", e.what());
                        responseJson = serverPtr->create_error_response(0, ErrorCodes::INTERNAL_ERROR,
                                                                      "Internal error", "");
                    }
                }
                if (responseJson.size() > serverPtr->m_policy.transport.max_response_bytes) {
                    responseJson = serverPtr->create_error_response(0, ErrorCodes::INVALID_REQUEST,
                                                                  "Payload too large", "");
                }
                co_await serverPtr->send_json_response(conn, responseJson);
            }

            // Keep-Alive: 循环处理后续请求，直到连接关闭
            auto reader = conn.get_reader();
            while (true) {
                http::HttpRequest nextReq;
                while (true) {
                    auto result = co_await reader.get_request(nextReq);
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

                const std::string& requestBody = nextReq.body_str();

                std::string responseJson;
                ++keepAliveRequests;
                if (keepAliveRequests > serverPtr->m_policy.transport.max_keep_alive_requests) {
                    responseJson = serverPtr->create_error_response(0, ErrorCodes::INVALID_REQUEST,
                                                                  "Keep-alive request limit exceeded", "");
                } else if (requestBody.size() > serverPtr->m_policy.transport.max_http_body_bytes) {
                    responseJson = serverPtr->create_error_response(0, ErrorCodes::INVALID_REQUEST,
                                                                  "Payload too large", "");
                } else {
                    try {
                        co_await serverPtr->process_request(requestBody, responseJson, connectionInitialized);
                    } catch (const std::exception& e) {
                        MCP_LOG_WARN("[http_server]", "keepalive request failed error={}", e.what());
                        responseJson = serverPtr->create_error_response(0, ErrorCodes::INTERNAL_ERROR,
                                                                      "Internal error", "");
                    }
                }
                if (responseJson.size() > serverPtr->m_policy.transport.max_response_bytes) {
                    responseJson = serverPtr->create_error_response(0, ErrorCodes::INVALID_REQUEST,
                                                                  "Payload too large", "");
                }
                co_await serverPtr->send_json_response(conn, responseJson);
            }
        });

    http::HttpServerConfig config;
    config.host = m_host;
    config.port = static_cast<uint16_t>(m_port);
    config.backlog = 128;
    config.io_scheduler_count = m_ioSchedulers;
    config.parallel_scheduler_count = m_parallelSchedulers;
    config.tcp_no_delay = m_tcpNoDelay;

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

bool McpHttpServer::is_running() const {
    return m_running;
}

galay::kernel::Task<void> McpHttpServer::send_json_response(http::HttpConn& conn, const std::string& responseJson) {
    std::string wireBytes;
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

    auto writer = conn.get_writer();
    while (true) {
        auto send_result = co_await writer.send(std::move(wireBytes));
        if (!send_result || send_result.value()) {
            break;
        }
    }
    co_return;
}

galay::kernel::Task<void> McpHttpServer::process_request(const std::string& requestBody, std::string& responseJson, bool& connectionInitialized) {
    try {
        auto parsed = parse_json_rpc_request(requestBody);
        if (!parsed) {
            MCP_LOG_WARN("[http_server]", "json-rpc request parse failed error={}",
                         parsed.error().details());
            responseJson = create_error_response(0,
                                   parsed.error().to_json_rpc_error_code(),
                                   parsed.error().message(),
                                   parsed.error().details());
            co_return;
        }

        const JsonRpcRequestView& request = parsed.value().request;
        const std::string& method = request.method;

        if (method == Methods::INITIALIZE) {
            responseJson = handle_initialize(request, connectionInitialized);
        } else if (method == Methods::TOOLS_LIST) {
            responseJson = handle_tools_list(request, connectionInitialized);
        } else if (method == Methods::TOOLS_CALL) {
            co_await handle_tools_call(request, responseJson, connectionInitialized);
        } else if (method == Methods::RESOURCES_LIST) {
            responseJson = handle_resources_list(request, connectionInitialized);
        } else if (method == Methods::RESOURCES_READ) {
            co_await handle_resources_read(request, responseJson, connectionInitialized);
        } else if (method == Methods::PROMPTS_LIST) {
            responseJson = handle_prompts_list(request, connectionInitialized);
        } else if (method == Methods::PROMPTS_GET) {
            co_await handle_prompts_get(request, responseJson, connectionInitialized);
        } else if (method == Methods::PING) {
            responseJson = handle_ping(request);
        } else {
            if (request.id.has_value()) {
                MCP_LOG_WARN("[http_server]", "method not found method={} id={}", method, request.id.value());
                responseJson = create_error_response(request.id.value(),
                                         ErrorCodes::METHOD_NOT_FOUND,
                                         "Method not found", method);
            } else {
                responseJson = empty_object_string();
            }
        }
    } catch (const std::exception& e) {
        MCP_LOG_WARN("[http_server]", "process request failed error={}", e.what());
        responseJson = create_error_response(0, ErrorCodes::INVALID_REQUEST,
                                  "Invalid request", "");
    }
    co_return;
}

std::string McpHttpServer::handle_initialize(const JsonRpcRequestView& request, bool& connectionInitialized) {
    if (!request.id.has_value()) {
        return empty_object_string();
    }

    if (connectionInitialized) {
        return create_error_response(request.id.value(), ErrorCodes::INVALID_REQUEST,
                                  "Already initialized", "");
    }

    if (!request.hasParams) {
        MCP_LOG_WARN("[http_server]", "initialize missing params id={}", request.id.value());
        return create_error_response(request.id.value(), ErrorCodes::INVALID_PARAMS,
                                  "Invalid parameters", "Missing params");
    }

    auto paramsExp = InitializeParams::from_json(request.params);
    if (!paramsExp) {
        MCP_LOG_WARN("[http_server]", "initialize params parse failed id={} error={}",
                     request.id.value(),
                     paramsExp.error().message());
        return create_error_response(request.id.value(), ErrorCodes::INVALID_PARAMS,
                                  "Invalid parameters", paramsExp.error().message());
    }

    std::string result = protocol::build_initialize_result(
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

    return make_result_response(request.id.value(), result);
}

std::string McpHttpServer::handle_tools_list(const JsonRpcRequestView& request, bool& connectionInitialized) {
    if (!request.id.has_value()) {
        return empty_object_string();
    }

    if (!connectionInitialized) {
        MCP_LOG_WARN("[http_server]", "tools/list before initialization id={}", request.id.value());
        return create_error_response(request.id.value(), ErrorCodes::INVALID_REQUEST,
                                  "Not initialized", "");
    }

    return make_result_response(request.id.value(), get_tools_list_result());
}

galay::kernel::Task<void> McpHttpServer::handle_tools_call(const JsonRpcRequestView& request, std::string& responseJson, bool& connectionInitialized) {
    if (!request.id.has_value()) {
        responseJson = empty_object_string();
        co_return;
    }

    if (!connectionInitialized) {
        MCP_LOG_WARN("[http_server]", "tools/call before initialization id={}", request.id.value());
        responseJson = create_error_response(request.id.value(), ErrorCodes::INVALID_REQUEST,
                                  "Not initialized", "");
        co_return;
    }

    try {
        if (!request.hasParams) {
            MCP_LOG_WARN("[http_server]", "tools/call missing params id={}", request.id.value());
            responseJson = create_error_response(request.id.value(), ErrorCodes::INVALID_PARAMS,
                                      "Invalid parameters", "Missing params");
            co_return;
        }

        json::Json paramsObj = request.params;
        if (!paramsObj.is_object()) {
            MCP_LOG_WARN("[http_server]", "tools/call params not object id={}", request.id.value());
            responseJson = create_error_response(request.id.value(), ErrorCodes::INVALID_PARAMS,
                                      "Invalid parameters", "Params must be object");
            co_return;
        }

        std::string toolName;
        auto nameVal = paramsObj.at("name").as_string();
        if (!nameVal) {
            MCP_LOG_WARN("[http_server]", "tools/call missing tool name id={}", request.id.value());
            responseJson = create_error_response(request.id.value(), ErrorCodes::INVALID_PARAMS,
                                      "Invalid parameters", "Missing tool name");
            co_return;
        }
        toolName = std::string(*nameVal);

        auto it = m_tools.find(toolName);
        if (it == m_tools.end()) {
            MCP_LOG_WARN("[http_server]", "tool not found id={} tool={}", request.id.value(), toolName);
            responseJson = create_error_response(request.id.value(), ErrorCodes::METHOD_NOT_FOUND,
                                      "Tool not found", toolName);
            co_return;
        }

        const auto& handler = it->second.handler;

        json::Json arguments = empty_json_object();
        json::Json argsElement = paramsObj.at("arguments");
        if (argsElement.valid()) {
            arguments = argsElement;
        }

        // 调用工具处理函数（协程）
        std::expected<std::string, McpError> result;
        co_await handler(arguments, result);

        if (!result) {
            MCP_LOG_WARN("[http_server]", "tool handler failed id={} tool={} error={}",
                         request.id.value(),
                         toolName,
                         result.error().message());
            responseJson = create_error_response(request.id.value(),
                                      result.error().to_json_rpc_error_code(),
                                      result.error().message(),
                                      result.error().details());
            co_return;
        }

        ToolCallResult callResult;
        Content content;
        content.type = ContentType::Text;
        content.text = result.value();
        callResult.content.push_back(content);

        responseJson = make_result_response(request.id.value(), callResult.to_json());

    } catch (const std::exception& e) {
        MCP_LOG_ERROR("[http_server]", "tools/call threw id={} error={}", request.id.value(), e.what());
        responseJson = create_error_response(request.id.value(), ErrorCodes::INTERNAL_ERROR,
                                  "Internal error", "");
    }
    co_return;
}

std::string McpHttpServer::handle_resources_list(const JsonRpcRequestView& request, bool& connectionInitialized) {
    if (!request.id.has_value()) {
        return empty_object_string();
    }

    if (!connectionInitialized) {
        MCP_LOG_WARN("[http_server]", "resources/list before initialization id={}", request.id.value());
        return create_error_response(request.id.value(), ErrorCodes::INVALID_REQUEST,
                                  "Not initialized", "");
    }

    return make_result_response(request.id.value(), get_resources_list_result());
}

galay::kernel::Task<void> McpHttpServer::handle_resources_read(const JsonRpcRequestView& request, std::string& responseJson, bool& connectionInitialized) {
    if (!request.id.has_value()) {
        responseJson = empty_object_string();
        co_return;
    }

    if (!connectionInitialized) {
        MCP_LOG_WARN("[http_server]", "resources/read before initialization id={}", request.id.value());
        responseJson = create_error_response(request.id.value(), ErrorCodes::INVALID_REQUEST,
                                  "Not initialized", "");
        co_return;
    }

    try {
        if (!request.hasParams) {
            MCP_LOG_WARN("[http_server]", "resources/read missing params id={}", request.id.value());
            responseJson = create_error_response(request.id.value(), ErrorCodes::INVALID_PARAMS,
                                      "Invalid parameters", "Missing params");
            co_return;
        }

        json::Json paramsObj = request.params;
        if (!paramsObj.is_object()) {
            MCP_LOG_WARN("[http_server]", "resources/read params not object id={}", request.id.value());
            responseJson = create_error_response(request.id.value(), ErrorCodes::INVALID_PARAMS,
                                      "Invalid parameters", "Params must be object");
            co_return;
        }

        std::string uri;
        auto uriVal = paramsObj.at("uri").as_string();
        if (!uriVal) {
            MCP_LOG_WARN("[http_server]", "resources/read missing uri id={}", request.id.value());
            responseJson = create_error_response(request.id.value(), ErrorCodes::INVALID_PARAMS,
                                      "Invalid parameters", "Missing uri");
            co_return;
        }
        uri = std::string(*uriVal);

        auto it = m_resources.find(uri);
        if (it == m_resources.end()) {
            MCP_LOG_WARN("[http_server]", "resource not found id={} uri={}", request.id.value(), uri);
            responseJson = create_error_response(request.id.value(), ErrorCodes::METHOD_NOT_FOUND,
                                      "Resource not found", uri);
            co_return;
        }

        const auto& reader = it->second.reader;

        // 调用资源读取函数（协程）
        std::expected<std::string, McpError> result;
        co_await reader(uri, result);

        if (!result) {
            MCP_LOG_WARN("[http_server]", "resource reader failed id={} uri={} error={}",
                         request.id.value(),
                         uri,
                         result.error().message());
            responseJson = create_error_response(request.id.value(),
                                      result.error().to_json_rpc_error_code(),
                                      result.error().message(),
                                      result.error().details());
            co_return;
        }

        Content content;
        content.type = ContentType::Text;
        content.text = result.value();

        std::string resultJson;
        auto resultWriter = make_json_writer(resultJson);
        // StreamWriter 失败粘滞：中间结果统一丢弃，由 finish() 统一检查
        (void)resultWriter.start_object();
        (void)resultWriter.key("contents");
        (void)resultWriter.start_array();
        (void)resultWriter.raw(content.to_json());
        (void)resultWriter.end_array();
        (void)resultWriter.end_object();
        if (!resultWriter.finish()) {
            MCP_LOG_ERROR("[http_server]", "result encode failed id={}", request.id.value());
            responseJson = create_error_response(request.id.value(), ErrorCodes::INTERNAL_ERROR,
                                      "Internal error", "");
            co_return;
        }

        responseJson = make_result_response(request.id.value(), std::move(resultJson));

    } catch (const std::exception& e) {
        MCP_LOG_ERROR("[http_server]", "resources/read threw id={} error={}", request.id.value(), e.what());
        responseJson = create_error_response(request.id.value(), ErrorCodes::INTERNAL_ERROR,
                                  "Internal error", "");
    }
    co_return;
}

std::string McpHttpServer::handle_prompts_list(const JsonRpcRequestView& request, bool& connectionInitialized) {
    if (!request.id.has_value()) {
        return empty_object_string();
    }

    if (!connectionInitialized) {
        MCP_LOG_WARN("[http_server]", "prompts/list before initialization id={}", request.id.value());
        return create_error_response(request.id.value(), ErrorCodes::INVALID_REQUEST,
                                  "Not initialized", "");
    }

    return make_result_response(request.id.value(), get_prompts_list_result());
}

galay::kernel::Task<void> McpHttpServer::handle_prompts_get(const JsonRpcRequestView& request, std::string& responseJson, bool& connectionInitialized) {
    if (!request.id.has_value()) {
        responseJson = empty_object_string();
        co_return;
    }

    if (!connectionInitialized) {
        MCP_LOG_WARN("[http_server]", "prompts/get before initialization id={}", request.id.value());
        responseJson = create_error_response(request.id.value(), ErrorCodes::INVALID_REQUEST,
                                  "Not initialized", "");
        co_return;
    }

    try {
        if (!request.hasParams) {
            MCP_LOG_WARN("[http_server]", "prompts/get missing params id={}", request.id.value());
            responseJson = create_error_response(request.id.value(), ErrorCodes::INVALID_PARAMS,
                                      "Invalid parameters", "Missing params");
            co_return;
        }

        json::Json paramsObj = request.params;
        if (!paramsObj.is_object()) {
            MCP_LOG_WARN("[http_server]", "prompts/get params not object id={}", request.id.value());
            responseJson = create_error_response(request.id.value(), ErrorCodes::INVALID_PARAMS,
                                      "Invalid parameters", "Params must be object");
            co_return;
        }

        std::string name;
        auto nameVal = paramsObj.at("name").as_string();
        if (!nameVal) {
            MCP_LOG_WARN("[http_server]", "prompts/get missing name id={}", request.id.value());
            responseJson = create_error_response(request.id.value(), ErrorCodes::INVALID_PARAMS,
                                      "Invalid parameters", "Missing prompt name");
            co_return;
        }
        name = std::string(*nameVal);

        json::Json arguments = empty_json_object();
        json::Json argsElement = paramsObj.at("arguments");
        if (argsElement.valid()) {
            arguments = argsElement;
        }

        auto it = m_prompts.find(name);
        if (it == m_prompts.end()) {
            MCP_LOG_WARN("[http_server]", "prompt not found id={} name={}", request.id.value(), name);
            responseJson = create_error_response(request.id.value(), ErrorCodes::METHOD_NOT_FOUND,
                                      "Prompt not found", name);
            co_return;
        }

        const auto& getter = it->second.getter;

        // 调用提示获取函数（协程）
        std::expected<std::string, McpError> result;
        co_await getter(name, arguments, result);

        if (!result) {
            MCP_LOG_WARN("[http_server]", "prompt getter failed id={} name={} error={}",
                         request.id.value(),
                         name,
                         result.error().message());
            responseJson = create_error_response(request.id.value(),
                                      result.error().to_json_rpc_error_code(),
                                      result.error().message(),
                                      result.error().details());
            co_return;
        }

        responseJson = make_result_response(request.id.value(), result.value());

    } catch (const std::exception& e) {
        MCP_LOG_ERROR("[http_server]", "prompts/get threw id={} error={}", request.id.value(), e.what());
        responseJson = create_error_response(request.id.value(), ErrorCodes::INTERNAL_ERROR,
                                  "Internal error", "");
    }
    co_return;
}

std::string McpHttpServer::handle_ping(const JsonRpcRequestView& request) {
    if (!request.id.has_value()) {
        return empty_object_string();
    }

    return make_result_response(request.id.value(), empty_object_string());
}

std::string McpHttpServer::create_error_response(int64_t id, int code,
                                        const std::string& message,
                                        const std::string& details) {
    return protocol::make_error_response(id, code, message, details).to_json();
}

const std::string& McpHttpServer::get_tools_list_result() {
    return m_toolsListCache;
}

const std::string& McpHttpServer::get_resources_list_result() {
    return m_resourcesListCache;
}

const std::string& McpHttpServer::get_prompts_list_result() {
    return m_promptsListCache;
}

} // namespace galay::mcp
