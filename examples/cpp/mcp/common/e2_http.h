#ifndef GALAY_MCP_EXAMPLE_E2_HTTP_H
#define GALAY_MCP_EXAMPLE_E2_HTTP_H

using namespace galay::mcp;
using namespace galay::kernel;

struct ClientRunState {
    std::atomic<bool> done{false};
};

static void finish_client_run(int code, int& exitCode, ClientRunState* state) {
    exitCode = code;
    state->done.store(true, std::memory_order_release);
}

/**
 * @brief 简单的HTTP服务器示例
 *
 * 创建一个HTTP MCP服务器，提供基本的工具和资源
 * @return 无返回值
 */
void run_http_server() {
    // 创建服务器（监听 0.0.0.0:8080）
    McpHttpServer server("0.0.0.0", 8080);

    // 设置服务器信息
    server.set_server_info("example-http-server", "1.0.0");

    // 添加一个简单的计算器工具
    auto calcSchema = SchemaBuilder()
        .add_enum("operation", "运算类型", {"add", "subtract", "multiply", "divide"}, true)
        .add_number("a", "第一个操作数", true)
        .add_number("b", "第二个操作数", true)
        .build();
    server.add_tool(
        "calculate",
        "执行基本的数学计算",
        calcSchema,
        [](const json::Json& args, std::expected<std::string, McpError>& result) -> galay::kernel::Task<void> {
            if (!args.is_object()) {
                result = std::unexpected(McpError(
                    McpErrorCode::InvalidParams,
                    "Invalid arguments"
                ));
                co_return;
            }

            auto op = args.at("operation").as_string();
            if (!op) {
                result = std::unexpected(McpError(
                    McpErrorCode::InvalidParams,
                    "Missing required parameters"
                ));
                co_return;
            }

            auto aVal = args.at("a");
            auto bVal = args.at("b");
            if (!aVal.valid() || !bVal.valid()) {
                result = std::unexpected(McpError(
                    McpErrorCode::InvalidParams,
                    "Missing required parameters"
                ));
                co_return;
            }

            auto aNum = aVal.as_double();
            auto bNum = bVal.as_double();
            if (!aNum) {
                result = std::unexpected(McpError(
                    McpErrorCode::InvalidParams,
                    "Invalid parameter 'a'"
                ));
                co_return;
            }

            if (!bNum) {
                result = std::unexpected(McpError(
                    McpErrorCode::InvalidParams,
                    "Invalid parameter 'b'"
                ));
                co_return;
            }

            double a = *aNum;
            double b = *bNum;
            std::string operation(*op);

            double answer = 0.0;

            if (operation == "add") {
                answer = a + b;
            } else if (operation == "subtract") {
                answer = a - b;
            } else if (operation == "multiply") {
                answer = a * b;
            } else if (operation == "divide") {
                if (b == 0) {
                    result = std::unexpected(McpError(
                        McpErrorCode::InvalidParams,
                        "Division by zero"
                    ));
                    co_return;
                }
                answer = a / b;
            } else {
                result = std::unexpected(McpError(
                    McpErrorCode::InvalidParams,
                    "Invalid operation"
                ));
                co_return;
            }

            std::string resultJson;
            auto resWriter = make_json_writer(resultJson);
            // StreamWriter 失败粘滞：中间结果统一丢弃，由 finish() 统一检查
            (void)resWriter.start_object();
            (void)resWriter.key("result");
            (void)resWriter.number(answer);
            (void)resWriter.key("operation");
            (void)resWriter.string(operation);
            (void)resWriter.end_object();
            if (!resWriter.finish()) {
                result = std::unexpected(McpError(
                    McpErrorCode::InternalError,
                    "Failed to encode result"
                ));
                co_return;
            }
            result = std::move(resultJson);
            co_return;
        }
    );

    // 添加一个时间资源
    server.add_resource(
        "example://time",
        "current-time",
        "获取当前时间",
        "text/plain",
        [](const std::string& uri, std::expected<std::string, McpError>& result) -> galay::kernel::Task<void> {
            auto now = std::chrono::system_clock::now();
            auto time_t = std::chrono::system_clock::to_time_t(now);
            result = std::ctime(&time_t);
            co_return;
        }
    );

    // 添加一个提示
    auto promptArgs = PromptArgumentBuilder()
        .add_argument("language", "编程语言", true)
        .build();
    server.add_prompt(
        "code_review",
        "生成代码审查提示",
        promptArgs,
        [](const std::string& name, const json::Json& args, std::expected<std::string, McpError>& result) -> galay::kernel::Task<void> {
            if (!args.is_object()) {
                result = std::unexpected(McpError(
                    McpErrorCode::InvalidParams,
                    "Invalid arguments"
                ));
                co_return;
            }

            auto lang = args.at("language").as_string();
            if (!lang) {
                result = std::unexpected(McpError(
                    McpErrorCode::InvalidParams,
                    "Missing 'language' parameter"
                ));
                co_return;
            }

            std::string resultJson;
            auto resWriter = make_json_writer(resultJson);
            // StreamWriter 失败粘滞：中间结果统一丢弃，由 finish() 统一检查
            (void)resWriter.start_object();
            (void)resWriter.key("description");
            (void)resWriter.string("Code review prompt for " + std::string(*lang));
            (void)resWriter.key("messages");
            (void)resWriter.start_array();
            (void)resWriter.start_object();
            (void)resWriter.key("role");
            (void)resWriter.string("user");
            (void)resWriter.key("content");
            (void)resWriter.string("Please review this " + std::string(*lang) + " code for best practices and potential issues.");
            (void)resWriter.end_object();
            (void)resWriter.end_array();
            (void)resWriter.end_object();
            if (!resWriter.finish()) {
                result = std::unexpected(McpError(
                    McpErrorCode::InternalError,
                    "Failed to encode result"
                ));
                co_return;
            }
            result = std::move(resultJson);
            co_return;
        }
    );

    // 启动服务器
    std::cout << "HTTP MCP Server starting on http://0.0.0.0:8080/mcp" << std::endl;
    std::cout << "Press Ctrl+C to stop" << std::endl;
    server.start();
}

// 客户端测试协程
galay::kernel::Task<void> run_client_test(McpClient& client,
                        const std::string& url,
                        int& exitCode,
                        ClientRunState* state) {
    // 连接到服务器
    std::cout << "Connecting to " << url << "..." << std::endl;
    auto connectResult = co_await client.connect();
    if (!connectResult) {
        std::cerr << "Failed to connect: " << connectResult.error().message() << std::endl;
        finish_client_run(1, exitCode, state);
        co_return;
    }

    // 初始化
    std::cout << "Initializing..." << std::endl;
    std::expected<void, McpError> initResult;
    co_await client.initialize("example-http-client", "1.0.0", initResult);
    if (!initResult) {
        std::cerr << "Failed to initialize: " << initResult.error().message() << std::endl;
        finish_client_run(1, exitCode, state);
        co_return;
    }

    std::cout << "Connected to: " << client.get_server_info().name << std::endl;

    // 列出工具
    std::cout << "\n=== Available Tools ===" << std::endl;
    std::expected<std::vector<Tool>, McpError> toolsResult;
    co_await client.list_tools(toolsResult);
    if (toolsResult) {
        for (const auto& tool : toolsResult.value()) {
            std::cout << "  - " << tool.name << ": " << tool.description << std::endl;
        }
    }

    // 调用计算器工具
    std::cout << "\n=== Calling Calculator Tool ===" << std::endl;
    std::string calcArgs;
    auto calcArgsWriter = make_json_writer(calcArgs);
    // StreamWriter 失败粘滞：中间结果统一丢弃，由 finish() 统一检查
    (void)calcArgsWriter.start_object();
    (void)calcArgsWriter.key("operation");
    (void)calcArgsWriter.string("multiply");
    (void)calcArgsWriter.key("a");
    (void)calcArgsWriter.number(static_cast<int64_t>(12));
    (void)calcArgsWriter.key("b");
    (void)calcArgsWriter.number(static_cast<int64_t>(8));
    (void)calcArgsWriter.end_object();
    if (!calcArgsWriter.finish()) {
        std::cerr << "Failed to encode calculator arguments" << std::endl;
        finish_client_run(1, exitCode, state);
        co_return;
    }
    std::expected<std::string, McpError> calcResult;
    co_await client.call_tool("calculate", calcArgs, calcResult);
    if (calcResult) {
        auto docExp = JsonDocument::parse(calcResult.value());
        if (docExp) {
            const json::Json& root = docExp.value().root();
            if (root.is_object()) {
                auto resultVal = root.at("result");
                if (resultVal.valid()) {
                    auto resultNum = resultVal.as_double();
                    if (resultNum) {
                        std::cout << "12 * 8 = " << *resultNum << std::endl;
                    } else {
                        std::cout << "Result: " << calcResult.value() << std::endl;
                    }
                }
            }
        } else {
            std::cout << "Result: " << calcResult.value() << std::endl;
        }
    }

    // 列出资源
    std::cout << "\n=== Available Resources ===" << std::endl;
    std::expected<std::vector<Resource>, McpError> resourcesResult;
    co_await client.list_resources(resourcesResult);
    if (resourcesResult) {
        for (const auto& resource : resourcesResult.value()) {
            std::cout << "  - " << resource.uri << ": " << resource.name << std::endl;
        }
    }

    // 读取时间资源
    std::cout << "\n=== Reading Time Resource ===" << std::endl;
    std::expected<std::string, McpError> timeResult;
    co_await client.read_resource("example://time", timeResult);
    if (timeResult) {
        std::cout << "Current time: " << timeResult.value();
    }

    // 列出提示
    std::cout << "\n=== Available Prompts ===" << std::endl;
    std::expected<std::vector<Prompt>, McpError> promptsResult;
    co_await client.list_prompts(promptsResult);
    if (promptsResult) {
        for (const auto& prompt : promptsResult.value()) {
            std::cout << "  - " << prompt.name << ": " << prompt.description << std::endl;
        }
    }

    // 获取提示
    std::cout << "\n=== Getting Code Review Prompt ===" << std::endl;
    std::string promptArgs;
    auto promptArgsWriter = make_json_writer(promptArgs);
    // StreamWriter 失败粘滞：中间结果统一丢弃，由 finish() 统一检查
    (void)promptArgsWriter.start_object();
    (void)promptArgsWriter.key("language");
    (void)promptArgsWriter.string("C++");
    (void)promptArgsWriter.end_object();
    if (!promptArgsWriter.finish()) {
        std::cerr << "Failed to encode prompt arguments" << std::endl;
        finish_client_run(1, exitCode, state);
        co_return;
    }
    std::expected<std::string, McpError> prompt_result;
    co_await client.get_prompt("code_review", promptArgs, prompt_result);
    if (prompt_result) {
        std::cout << "Prompt: " << prompt_result.value() << std::endl;
    }

    // 测试ping
    std::cout << "\n=== Testing Ping ===" << std::endl;
    std::expected<void, McpError> pingResult;
    co_await client.ping(pingResult);
    if (pingResult) {
        std::cout << "Ping successful!" << std::endl;
    }

    // 断开连接
    co_await client.disconnect_async();
    std::cout << "\nClient disconnected." << std::endl;

    finish_client_run(0, exitCode, state);
    co_return;
}

/**
 * @brief 简单的HTTP客户端示例
 *
 * 创建一个HTTP MCP客户端，连接到服务器并调用功能
 * @param url 目标 URL
 * @return 进程退出码；0 表示成功，非 0 表示失败
 */
int run_http_client(const std::string& url) {
    // 创建Runtime
    Runtime runtime = RuntimeBuilder().io_scheduler_count(1).parallel_scheduler_count(1).build();
    runtime.start();

    // 创建客户端
    McpClient client(runtime, McpHttpClientConfig{.url = url});

    int exitCode = 0;
    ClientRunState state;

    // 在IO调度器上运行测试协程
    auto* scheduler = runtime.get_next_io_scheduler();
    if (!scheduler || !schedule_task(scheduler, run_client_test(client, url, exitCode, &state))) {
        std::cerr << "Failed to schedule HTTP MCP client task" << std::endl;
        runtime.stop();
        return 1;
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (!state.done.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    // 停止Runtime
    runtime.stop();
    if (!state.done.load(std::memory_order_acquire)) {
        std::cerr << "HTTP MCP client example timed out" << std::endl;
        return 1;
    }
    return exitCode;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cout << "Usage:" << std::endl;
        std::cout << "  " << argv[0] << " server              - Run as server" << std::endl;
        std::cout << "  " << argv[0] << " client [url]        - Run as client" << std::endl;
        std::cout << std::endl;
        std::cout << "Example:" << std::endl;
        std::cout << "  Terminal 1: " << argv[0] << " server" << std::endl;
        std::cout << "  Terminal 2: " << argv[0] << " client http://127.0.0.1:8080/mcp" << std::endl;
        return 1;
    }

    std::string mode = argv[1];

    if (mode == "server") {
        run_http_server();
    } else if (mode == "client") {
        std::string url = "http://127.0.0.1:8080/mcp";
        if (argc > 2) {
            url = argv[2];
        }
        return run_http_client(url);
    } else {
        std::cerr << "Invalid mode: " << mode << std::endl;
        std::cerr << "Use 'server' or 'client'" << std::endl;
        return 1;
    }

    return 0;
}

#endif  // GALAY_MCP_EXAMPLE_E2_HTTP_H
