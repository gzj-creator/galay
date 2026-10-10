#include "stdio_server.h"
#include "../../common/mcp_log.h"
#include "../../common/protocol_utils.h"
#include <sstream>
#include <stdexcept>
#include <mutex>

namespace galay::mcp {

namespace {

std::string empty_object_string() {
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

McpStdioServer::~McpStdioServer() {
    stop();
}

void McpStdioServer::set_server_info(const std::string& name, const std::string& version) {
    m_serverName = name;
    m_serverVersion = version;
}

void McpStdioServer::set_production_policy(McpProductionPolicy policy) {
    m_policy = std::move(policy);
}

void McpStdioServer::set_streams(std::istream& input, std::ostream& output) noexcept {
    m_input = &input;
    m_output = &output;
}

void McpStdioServer::add_tool(std::string name,
                             std::string description,
                             std::string inputSchema,
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
    m_toolsListCache = protocol::build_list_result_from_map(
        m_tools, "tools",
        [](const ToolInfo& info) -> const Tool& { return info.tool; });
}

void McpStdioServer::add_resource(std::string uri,
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
    m_resourcesListCache = protocol::build_list_result_from_map(
        m_resources, "resources",
        [](const ResourceInfo& info) -> const Resource& { return info.resource; });
}

void McpStdioServer::add_prompt(std::string name,
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
    m_promptsListCache = protocol::build_list_result_from_map(
        m_prompts, "prompts",
        [](const PromptInfo& info) -> const Prompt& { return info.prompt; });
}

void McpStdioServer::run() {
    m_running = true;
    MCP_LOG_INFO("[stdio_server]", "run loop started server={}/{}", m_serverName, m_serverVersion);

    while (m_running) {
        auto messageResult = read_message();
        if (!messageResult) {
            if (messageResult.error().code() == McpErrorCode::PayloadTooLarge) {
                send_error(0, messageResult.error().to_json_rpc_error_code(), messageResult.error().message(), "");
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

        auto parsed = parse_json_rpc_request(messageResult.value());
        if (!parsed) {
            MCP_LOG_WARN("[stdio_server]", "json-rpc request parse failed error={}",
                         parsed.error().details());
            send_error(0, ErrorCodes::PARSE_ERROR, "Parse error", parsed.error().details());
            continue;
        }

        handle_request(parsed.value().request);
    }

    m_running = false;
}

void McpStdioServer::stop() {
    if (m_running) {
        MCP_LOG_INFO("[stdio_server]", "run loop stopping");
    }
    m_running = false;
}

bool McpStdioServer::is_running() const {
    return m_running;
}

void McpStdioServer::handle_request(const JsonRpcRequestView& request) {
    const std::string& method = request.method;

    if (method == Methods::INITIALIZE) {
        handle_initialize(request);
    } else if (method == Methods::TOOLS_LIST) {
        handle_tools_list(request);
    } else if (method == Methods::TOOLS_CALL) {
        handle_tools_call(request);
    } else if (method == Methods::RESOURCES_LIST) {
        handle_resources_list(request);
    } else if (method == Methods::RESOURCES_READ) {
        handle_resources_read(request);
    } else if (method == Methods::PROMPTS_LIST) {
        handle_prompts_list(request);
    } else if (method == Methods::PROMPTS_GET) {
        handle_prompts_get(request);
    } else if (method == Methods::PING) {
        handle_ping(request);
    } else {
        if (request.id.has_value()) {
            MCP_LOG_WARN("[stdio_server]", "method not found method={} id={}", method, request.id.value());
            send_error(request.id.value(), ErrorCodes::METHOD_NOT_FOUND,
                     "Method not found", method);
        }
    }
}

void McpStdioServer::handle_initialize(const JsonRpcRequestView& request) {
    if (!request.id.has_value()) {
        return;
    }

    if (m_initialized) {
        MCP_LOG_WARN("[stdio_server]", "initialize rejected: already initialized id={}", request.id.value());
        send_error(request.id.value(), ErrorCodes::INVALID_REQUEST,
                 "Already initialized", "");
        return;
    }

    if (!request.hasParams) {
        MCP_LOG_WARN("[stdio_server]", "initialize missing params id={}", request.id.value());
        send_error(request.id.value(), ErrorCodes::INVALID_PARAMS,
                 "Invalid parameters", "Missing params");
        return;
    }

    auto paramsExp = InitializeParams::decode(request.params);
    if (!paramsExp) {
        MCP_LOG_WARN("[stdio_server]", "initialize params parse failed id={} error={}",
                     request.id.value(),
                     paramsExp.error().message());
        send_error(request.id.value(), ErrorCodes::INVALID_PARAMS,
                 "Invalid parameters", paramsExp.error().message());
        return;
    }

    // 构建响应
    std::string result = protocol::build_initialize_result(
        m_serverName,
        m_serverVersion,
        !m_tools.empty(),
        !m_resources.empty(),
        !m_prompts.empty());

    JsonRpcResponse response = protocol::make_result_response(request.id.value(), result);

    send_response(response);

    m_initialized = true;
    MCP_LOG_INFO("[stdio_server]", "initialized id={} server={}/{}",
                 request.id.value(),
                 m_serverName,
                 m_serverVersion);

    // 发送initialized通知
    send_notification(Methods::INITIALIZED, empty_object_string());
}

void McpStdioServer::handle_tools_list(const JsonRpcRequestView& request) {
    if (!request.id.has_value()) {
        return;
    }

    if (!m_initialized) {
        MCP_LOG_WARN("[stdio_server]", "tools/list before initialization id={}", request.id.value());
        send_error(request.id.value(), ErrorCodes::INVALID_REQUEST,
                 "Not initialized", "");
        return;
    }

    std::shared_lock<std::shared_mutex> lock(m_toolsMutex);

    JsonRpcResponse response = protocol::make_result_response(
        request.id.value(), m_toolsListCache);

    send_response(response);
}

void McpStdioServer::handle_tools_call(const JsonRpcRequestView& request) {
    if (!request.id.has_value()) {
        return;
    }

    if (!m_initialized) {
        MCP_LOG_WARN("[stdio_server]", "tools/call before initialization id={}", request.id.value());
        send_error(request.id.value(), ErrorCodes::INVALID_REQUEST,
                 "Not initialized", "");
        return;
    }

    try {
        if (!request.hasParams) {
            MCP_LOG_WARN("[stdio_server]", "tools/call missing params id={}", request.id.value());
            send_error(request.id.value(), ErrorCodes::INVALID_PARAMS,
                     "Invalid parameters", "Missing params");
            return;
        }

        auto params = json::decode<NamedArguments>(request.params);
        if (!params) {
            send_error(request.id.value(), ErrorCodes::INVALID_PARAMS,
                     "Invalid parameters", params.error());
            return;
        }
        const std::string& toolName = params->name;

        std::shared_lock<std::shared_mutex> lock(m_toolsMutex);

        auto it = m_tools.find(toolName);
        if (it == m_tools.end()) {
            MCP_LOG_WARN("[stdio_server]", "tool not found id={} tool={}", request.id.value(), toolName);
            send_error(request.id.value(), ErrorCodes::METHOD_NOT_FOUND,
                     "Tool not found", toolName);
            return;
        }


        // 调用工具处理函数
        auto result = it->second.handler(params->arguments);

        if (!result) {
            MCP_LOG_WARN("[stdio_server]", "tool handler failed id={} tool={} error={}",
                         request.id.value(),
                         toolName,
                         result.error().message());
            send_error(request.id.value(), result.error().to_json_rpc_error_code(),
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
        response.result = callResult.encode();

        send_response(response);

    } catch (const std::exception& e) {
        MCP_LOG_ERROR("[stdio_server]", "tools/call threw id={} error={}", request.id.value(), e.what());
        send_error(request.id.value(), ErrorCodes::INTERNAL_ERROR,
                 "Internal error", "");
    }
}

void McpStdioServer::handle_resources_list(const JsonRpcRequestView& request) {
    if (!request.id.has_value()) {
        return;
    }

    if (!m_initialized) {
        MCP_LOG_WARN("[stdio_server]", "resources/list before initialization id={}", request.id.value());
        send_error(request.id.value(), ErrorCodes::INVALID_REQUEST,
                 "Not initialized", "");
        return;
    }

    std::shared_lock<std::shared_mutex> lock(m_resourcesMutex);

    JsonRpcResponse response = protocol::make_result_response(
        request.id.value(), m_resourcesListCache);

    send_response(response);
}

void McpStdioServer::handle_resources_read(const JsonRpcRequestView& request) {
    if (!request.id.has_value()) {
        return;
    }

    if (!m_initialized) {
        MCP_LOG_WARN("[stdio_server]", "resources/read before initialization id={}", request.id.value());
        send_error(request.id.value(), ErrorCodes::INVALID_REQUEST,
                 "Not initialized", "");
        return;
    }

    try {
        if (!request.hasParams) {
            MCP_LOG_WARN("[stdio_server]", "resources/read missing params id={}", request.id.value());
            send_error(request.id.value(), ErrorCodes::INVALID_PARAMS,
                     "Invalid parameters", "Missing params");
            return;
        }

        auto params = json::decode<ResourceParams>(request.params);
        if (!params) {
            send_error(request.id.value(), ErrorCodes::INVALID_PARAMS,
                     "Invalid parameters", params.error());
            return;
        }
        const std::string& uri = params->uri;

        std::shared_lock<std::shared_mutex> lock(m_resourcesMutex);

        auto it = m_resources.find(uri);
        if (it == m_resources.end()) {
            MCP_LOG_WARN("[stdio_server]", "resource not found id={} uri={}", request.id.value(), uri);
            send_error(request.id.value(), ErrorCodes::METHOD_NOT_FOUND,
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
            send_error(request.id.value(), result.error().to_json_rpc_error_code(),
                     result.error().message(), result.error().details());
            return;
        }

        // 构建响应
        Content content;
        content.type = ContentType::Text;
        content.text = result.value();

        auto result_body = json::serialize(std::map<std::string, std::vector<Content>>{{"contents", {content}}});
        if (!result_body) {
            MCP_LOG_ERROR("[stdio_server]", "result encode failed id={}", request.id.value());
            send_error(request.id.value(), ErrorCodes::INTERNAL_ERROR,
                     "Internal error", "");
            return;
        }

        JsonRpcResponse response;
        response.id = request.id.value();
        response.result = std::move(*result_body);

        send_response(response);

    } catch (const std::exception& e) {
        MCP_LOG_ERROR("[stdio_server]", "resources/read threw id={} error={}", request.id.value(), e.what());
        send_error(request.id.value(), ErrorCodes::INTERNAL_ERROR,
                 "Internal error", "");
    }
}

void McpStdioServer::handle_prompts_list(const JsonRpcRequestView& request) {
    if (!request.id.has_value()) {
        return;
    }

    if (!m_initialized) {
        MCP_LOG_WARN("[stdio_server]", "prompts/list before initialization id={}", request.id.value());
        send_error(request.id.value(), ErrorCodes::INVALID_REQUEST,
                 "Not initialized", "");
        return;
    }

    std::shared_lock<std::shared_mutex> lock(m_promptsMutex);

    JsonRpcResponse response = protocol::make_result_response(
        request.id.value(), m_promptsListCache);

    send_response(response);
}

void McpStdioServer::handle_prompts_get(const JsonRpcRequestView& request) {
    if (!request.id.has_value()) {
        return;
    }

    if (!m_initialized) {
        MCP_LOG_WARN("[stdio_server]", "prompts/get before initialization id={}", request.id.value());
        send_error(request.id.value(), ErrorCodes::INVALID_REQUEST,
                 "Not initialized", "");
        return;
    }

    try {
        if (!request.hasParams) {
            MCP_LOG_WARN("[stdio_server]", "prompts/get missing params id={}", request.id.value());
            send_error(request.id.value(), ErrorCodes::INVALID_PARAMS,
                     "Invalid parameters", "Missing params");
            return;
        }

        auto params = json::decode<NamedArguments>(request.params);
        if (!params) {
            send_error(request.id.value(), ErrorCodes::INVALID_PARAMS,
                     "Invalid parameters", params.error());
            return;
        }
        const std::string& name = params->name;

        std::shared_lock<std::shared_mutex> lock(m_promptsMutex);

        auto it = m_prompts.find(name);
        if (it == m_prompts.end()) {
            MCP_LOG_WARN("[stdio_server]", "prompt not found id={} name={}", request.id.value(), name);
            send_error(request.id.value(), ErrorCodes::METHOD_NOT_FOUND,
                     "Prompt not found", name);
            return;
        }

        // 调用提示获取函数
        auto result = it->second.getter(name, params->arguments);

        if (!result) {
            MCP_LOG_WARN("[stdio_server]", "prompt getter failed id={} name={} error={}",
                         request.id.value(),
                         name,
                         result.error().message());
            send_error(request.id.value(), result.error().to_json_rpc_error_code(),
                     result.error().message(), result.error().details());
            return;
        }

        JsonRpcResponse response;
        response.id = request.id.value();
        response.result = result.value();

        send_response(response);

    } catch (const std::exception& e) {
        MCP_LOG_ERROR("[stdio_server]", "prompts/get threw id={} error={}", request.id.value(), e.what());
        send_error(request.id.value(), ErrorCodes::INTERNAL_ERROR,
                 "Internal error", "");
    }
}

void McpStdioServer::handle_ping(const JsonRpcRequestView& request) {
    if (!request.id.has_value()) {
        return;
    }

    JsonRpcResponse response = protocol::make_result_response(
        request.id.value(), empty_object_string());

    send_response(response);
}

void McpStdioServer::send_response(const JsonRpcResponse& response) {
    std::string message = response.encode();
    if (message.size() > m_policy.transport.max_response_bytes) {
        message = protocol::make_error_response(
            response.id,
            ErrorCodes::INVALID_REQUEST,
            "Payload too large",
            "").encode();
    }

    auto result = write_message(message);
    if (!result) {
        MCP_LOG_ERROR("[stdio_server]", "write response failed id={} error={}",
                      response.id,
                      result.error().message());
    }
}

void McpStdioServer::send_error(int64_t id, int code, const std::string& message,
                               const std::string& details) {
    send_response(protocol::make_error_response(id, code, message, details));
}

void McpStdioServer::send_notification(const std::string& method, const std::string& params) {
    JsonRpcNotification notification;
    notification.method = method;
    notification.params = params;

    auto result = write_message(notification.encode());
    if (!result) {
        MCP_LOG_ERROR("[stdio_server]", "write notification failed method={} error={}",
                      method,
                      result.error().message());
    }
}

std::expected<std::string, McpError> McpStdioServer::read_message() {
    std::string line;
    if (!std::getline(*m_input, line)) {
        return std::unexpected(McpError::read_error("Failed to read from stdin"));
    }

    if (line.empty()) {
        return std::unexpected(McpError::invalid_message("Empty message"));
    }
    if (line.size() > m_policy.transport.max_stdio_line_bytes) {
        return std::unexpected(McpError::payload_too_large("stdio line exceeds configured limit"));
    }

    return line;
}

std::expected<void, McpError> McpStdioServer::write_message(const std::string& message) {
    std::lock_guard<std::mutex> lock(m_outputMutex);

    try {
        *m_output << message << '\n';
        m_output->flush();
        return {};
    } catch (const std::exception& e) {
        return std::unexpected(McpError::write_error(e.what()));
    }
}

} // namespace galay::mcp
