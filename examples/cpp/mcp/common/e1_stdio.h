#ifndef GALAY_MCP_EXAMPLE_E1_STDIO_H
#define GALAY_MCP_EXAMPLE_E1_STDIO_H

using namespace galay::mcp;

/**
 * @brief 简单的服务器示例
 *
 * 创建一个MCP服务器，提供基本的工具和资源
 */
void run_simple_server() {
    McpStdioServer server;

    // 设置服务器信息
    server.set_server_info("example-server", "1.0.0");

    // 添加一个简单的echo工具
    auto echoSchema = SchemaBuilder()
        .add_string("message", "要回显的消息", true)
        .build();
    server.add_tool(
        "echo",
        "回显输入的消息",
        echoSchema,
        [](const json::Json& args) -> std::expected<std::string, McpError> {
            if (!args.is_object()) {
                return std::unexpected(McpError(
                    McpErrorCode::InvalidParams,
                    "Invalid arguments"
                ));
            }

            auto message = args.at("message").as_string();
            if (!message) {
                return std::unexpected(McpError(
                    McpErrorCode::InvalidParams,
                    "Missing 'message' parameter"
                ));
            }

            std::string result;
            auto writer = make_json_writer(result);
            // StreamWriter 失败粘滞：中间结果统一丢弃，由 finish() 统一检查
            (void)writer.start_object();
            (void)writer.key("echo");
            (void)writer.string(*message);
            (void)writer.end_object();
            if (!writer.finish()) {
                return std::unexpected(McpError(
                    McpErrorCode::InternalError,
                    "Failed to encode result"
                ));
            }
            return result;
        }
    );

    // 添加一个简单的资源
    server.add_resource(
        "example://greeting",
        "greeting",
        "简单的问候资源",
        "text/plain",
        [](const std::string& uri) -> std::expected<std::string, McpError> {
            return "Hello from MCP Server!";
        }
    );

    // 运行服务器（阻塞）
    std::cerr << "Server started. Waiting for requests..." << std::endl;
    server.run();
}

/**
 * @brief 简单的客户端示例
 *
 * 创建一个MCP客户端，连接到服务器并调用功能
 */
void run_simple_client() {
    McpClient client(McpStdioClientConfig{});

    // 初始化连接
    std::cout << "Initializing client..." << std::endl;
    auto initResult = client.initialize("example-client", "1.0.0");
    if (!initResult) {
        std::cerr << "Failed to initialize: " << initResult.error().to_string() << std::endl;
        return;
    }

    std::cout << "Connected to server: " << client.get_server_info().name << std::endl;

    // 列出可用工具
    std::cout << "\nListing tools..." << std::endl;
    auto toolsResult = client.list_tools();
    if (toolsResult) {
        for (const auto& tool : toolsResult.value()) {
            std::cout << "  - " << tool.name << ": " << tool.description << std::endl;
        }
    }

    // 调用echo工具
    std::cout << "\nCalling echo tool..." << std::endl;
    std::string args;
    auto argsWriter = make_json_writer(args);
    // StreamWriter 失败粘滞：中间结果统一丢弃，由 finish() 统一检查
    (void)argsWriter.start_object();
    (void)argsWriter.key("message");
    (void)argsWriter.string("Hello, MCP!");
    (void)argsWriter.end_object();
    if (!argsWriter.finish()) {
        std::cerr << "Failed to encode arguments" << std::endl;
        return;
    }
    auto callResult = client.call_tool("echo", args);
    if (callResult) {
        std::cout << "Result: " << callResult.value() << std::endl;
    }

    // 列出资源
    std::cout << "\nListing resources..." << std::endl;
    auto resourcesResult = client.list_resources();
    if (resourcesResult) {
        for (const auto& resource : resourcesResult.value()) {
            std::cout << "  - " << resource.uri << ": " << resource.name << std::endl;
        }
    }

    // 读取资源
    std::cout << "\nReading resource..." << std::endl;
    auto readResult = client.read_resource("example://greeting");
    if (readResult) {
        std::cout << "Content: " << readResult.value() << std::endl;
    }

    // 断开连接
    client.disconnect();
    std::cout << "\nClient disconnected." << std::endl;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cout << "Usage:" << std::endl;
        std::cout << "  " << argv[0] << " server  - Run as server" << std::endl;
        std::cout << "  " << argv[0] << " client  - Run as client" << std::endl;
        std::cout << std::endl;
        std::cout << "Example:" << std::endl;
        std::cout << "  Terminal 1: " << argv[0] << " server" << std::endl;
        std::cout << "  Terminal 2: " << argv[0] << " client | " << argv[0] << " server" << std::endl;
        return 1;
    }

    std::string mode = argv[1];

    if (mode == "server") {
        run_simple_server();
    } else if (mode == "client") {
        run_simple_client();
    } else {
        std::cerr << "Invalid mode: " << mode << std::endl;
        std::cerr << "Use 'server' or 'client'" << std::endl;
        return 1;
    }

    return 0;
}

#endif  // GALAY_MCP_EXAMPLE_E1_STDIO_H
