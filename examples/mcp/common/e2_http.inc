using namespace galay::mcp;
using namespace galay::kernel;

struct ClientRunState {
    std::atomic<bool> done{false};
};

/**
 * @brief 简单的HTTP服务器示例
 *
 * 创建一个HTTP MCP服务器，提供基本的工具和资源
 */
void runHttpServer() {
    // 创建服务器（监听 0.0.0.0:8080）
    McpHttpServer server("0.0.0.0", 8080);

    // 设置服务器信息
    server.setServerInfo("example-http-server", "1.0.0");

    // 添加一个简单的计算器工具
    auto calcSchema = SchemaBuilder()
        .addEnum("operation", "运算类型", {"add", "subtract", "multiply", "divide"}, true)
        .addNumber("a", "第一个操作数", true)
        .addNumber("b", "第二个操作数", true)
        .build();
    server.addTool(
        "calculate",
        "执行基本的数学计算",
        calcSchema,
        [](const JsonElement& args, std::expected<JsonString, McpError>& result) -> Coroutine {
            JsonObject obj;
            if (!JsonHelper::GetObject(args, obj)) {
                result = std::unexpected(McpError(
                    McpErrorCode::InvalidParams,
                    "Invalid arguments"
                ));
                co_return;
            }

            std::string op;
            if (!JsonHelper::GetString(obj, "operation", op)) {
                result = std::unexpected(McpError(
                    McpErrorCode::InvalidParams,
                    "Missing required parameters"
                ));
                co_return;
            }

            auto aVal = obj["a"];
            auto bVal = obj["b"];
            if (aVal.error() || bVal.error()) {
                result = std::unexpected(McpError(
                    McpErrorCode::InvalidParams,
                    "Missing required parameters"
                ));
                co_return;
            }

            double a = 0.0;
            double b = 0.0;
            if (aVal.is_double()) {
                a = aVal.get_double().value();
            } else if (aVal.is_int64()) {
                a = static_cast<double>(aVal.get_int64().value());
            } else {
                result = std::unexpected(McpError(
                    McpErrorCode::InvalidParams,
                    "Invalid parameter 'a'"
                ));
                co_return;
            }

            if (bVal.is_double()) {
                b = bVal.get_double().value();
            } else if (bVal.is_int64()) {
                b = static_cast<double>(bVal.get_int64().value());
            } else {
                result = std::unexpected(McpError(
                    McpErrorCode::InvalidParams,
                    "Invalid parameter 'b'"
                ));
                co_return;
            }

            double answer = 0.0;

            if (op == "add") {
                answer = a + b;
            } else if (op == "subtract") {
                answer = a - b;
            } else if (op == "multiply") {
                answer = a * b;
            } else if (op == "divide") {
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

            JsonWriter resWriter;
            resWriter.StartObject();
            resWriter.Key("result");
            resWriter.Number(answer);
            resWriter.Key("operation");
            resWriter.String(op);
            resWriter.EndObject();
            result = resWriter.TakeString();
            co_return;
        }
    );

    // 添加一个时间资源
    server.addResource(
        "example://time",
        "current-time",
        "获取当前时间",
        "text/plain",
        [](const std::string& uri, std::expected<std::string, McpError>& result) -> Coroutine {
            auto now = std::chrono::system_clock::now();
            auto time_t = std::chrono::system_clock::to_time_t(now);
            result = std::ctime(&time_t);
            co_return;
        }
    );

    // 添加一个提示
    auto promptArgs = PromptArgumentBuilder()
        .addArgument("language", "编程语言", true)
        .build();
    server.addPrompt(
        "code_review",
        "生成代码审查提示",
        promptArgs,
        [](const std::string& name, const JsonElement& args, std::expected<JsonString, McpError>& result) -> Coroutine {
            JsonObject obj;
            if (!JsonHelper::GetObject(args, obj)) {
                result = std::unexpected(McpError(
                    McpErrorCode::InvalidParams,
                    "Invalid arguments"
                ));
                co_return;
            }

            std::string lang;
            if (!JsonHelper::GetString(obj, "language", lang)) {
                result = std::unexpected(McpError(
                    McpErrorCode::InvalidParams,
                    "Missing 'language' parameter"
                ));
                co_return;
            }

            JsonWriter resWriter;
            resWriter.StartObject();
            resWriter.Key("description");
            resWriter.String("Code review prompt for " + lang);
            resWriter.Key("messages");
            resWriter.StartArray();
            resWriter.StartObject();
            resWriter.Key("role");
            resWriter.String("user");
            resWriter.Key("content");
            resWriter.String("Please review this " + lang + " code for best practices and potential issues.");
            resWriter.EndObject();
            resWriter.EndArray();
            resWriter.EndObject();
            result = resWriter.TakeString();
            co_return;
        }
    );

    // 启动服务器
    std::cout << "HTTP MCP Server starting on http://0.0.0.0:8080/mcp" << std::endl;
    std::cout << "Press Ctrl+C to stop" << std::endl;
    server.start();
}

// 客户端测试协程
Coroutine runClientTest(McpHttpClient& client,
                        const std::string& url,
                        int& exitCode,
                        ClientRunState* state) {
    auto finish = [&](int code) {
        exitCode = code;
        state->done.store(true, std::memory_order_release);
    };

    // 连接到服务器
    std::cout << "Connecting to " << url << "..." << std::endl;
    auto connectResult = co_await client.connect(url);
    if (!connectResult) {
        std::cerr << "Failed to connect: " << connectResult.error().message() << std::endl;
        finish(1);
        co_return;
    }

    // 初始化
    std::cout << "Initializing..." << std::endl;
    std::expected<void, McpError> initResult;
    co_await client.initialize("example-http-client", "1.0.0", initResult);
    if (!initResult) {
        std::cerr << "Failed to initialize: " << initResult.error().message() << std::endl;
        finish(1);
        co_return;
    }

    std::cout << "Connected to: " << client.getServerInfo().name << std::endl;

    // 列出工具
    std::cout << "\n=== Available Tools ===" << std::endl;
    std::expected<std::vector<Tool>, McpError> toolsResult;
    co_await client.listTools(toolsResult);
    if (toolsResult) {
        for (const auto& tool : toolsResult.value()) {
            std::cout << "  - " << tool.name << ": " << tool.description << std::endl;
        }
    }

    // 调用计算器工具
    std::cout << "\n=== Calling Calculator Tool ===" << std::endl;
    JsonWriter calcArgsWriter;
    calcArgsWriter.StartObject();
    calcArgsWriter.Key("operation");
    calcArgsWriter.String("multiply");
    calcArgsWriter.Key("a");
    calcArgsWriter.Number(static_cast<int64_t>(12));
    calcArgsWriter.Key("b");
    calcArgsWriter.Number(static_cast<int64_t>(8));
    calcArgsWriter.EndObject();
    std::expected<JsonString, McpError> calcResult;
    co_await client.callTool("calculate", calcArgsWriter.TakeString(), calcResult);
    if (calcResult) {
        auto docExp = JsonDocument::Parse(calcResult.value());
        if (docExp) {
            JsonObject obj;
            if (JsonHelper::GetObject(docExp.value().Root(), obj)) {
                auto resultVal = obj["result"];
                if (!resultVal.error()) {
                    if (resultVal.is_double()) {
                        std::cout << "12 * 8 = " << resultVal.get_double().value() << std::endl;
                    } else if (resultVal.is_int64()) {
                        std::cout << "12 * 8 = " << resultVal.get_int64().value() << std::endl;
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
    co_await client.listResources(resourcesResult);
    if (resourcesResult) {
        for (const auto& resource : resourcesResult.value()) {
            std::cout << "  - " << resource.uri << ": " << resource.name << std::endl;
        }
    }

    // 读取时间资源
    std::cout << "\n=== Reading Time Resource ===" << std::endl;
    std::expected<std::string, McpError> timeResult;
    co_await client.readResource("example://time", timeResult);
    if (timeResult) {
        std::cout << "Current time: " << timeResult.value();
    }

    // 列出提示
    std::cout << "\n=== Available Prompts ===" << std::endl;
    std::expected<std::vector<Prompt>, McpError> promptsResult;
    co_await client.listPrompts(promptsResult);
    if (promptsResult) {
        for (const auto& prompt : promptsResult.value()) {
            std::cout << "  - " << prompt.name << ": " << prompt.description << std::endl;
        }
    }

    // 获取提示
    std::cout << "\n=== Getting Code Review Prompt ===" << std::endl;
    JsonWriter promptArgsWriter;
    promptArgsWriter.StartObject();
    promptArgsWriter.Key("language");
    promptArgsWriter.String("C++");
    promptArgsWriter.EndObject();
    std::expected<JsonString, McpError> promptResult;
    co_await client.getPrompt("code_review", promptArgsWriter.TakeString(), promptResult);
    if (promptResult) {
        std::cout << "Prompt: " << promptResult.value() << std::endl;
    }

    // 测试ping
    std::cout << "\n=== Testing Ping ===" << std::endl;
    std::expected<void, McpError> pingResult;
    co_await client.ping(pingResult);
    if (pingResult) {
        std::cout << "Ping successful!" << std::endl;
    }

    // 断开连接
    co_await client.disconnect();
    std::cout << "\nClient disconnected." << std::endl;

    finish(0);
    co_return;
}

/**
 * @brief 简单的HTTP客户端示例
 *
 * 创建一个HTTP MCP客户端，连接到服务器并调用功能
 */
int runHttpClient(const std::string& url) {
    // 创建Runtime
    Runtime runtime = RuntimeBuilder().ioSchedulerCount(1).computeSchedulerCount(1).build();
    runtime.start();

    // 创建客户端
    McpHttpClient client(runtime);

    int exitCode = 0;
    ClientRunState state;

    // 在IO调度器上运行测试协程
    auto* scheduler = runtime.getNextIOScheduler();
    if (!scheduler || !scheduleTask(scheduler, runClientTest(client, url, exitCode, &state))) {
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
        runHttpServer();
    } else if (mode == "client") {
        std::string url = "http://127.0.0.1:8080/mcp";
        if (argc > 2) {
            url = argv[2];
        }
        return runHttpClient(url);
    } else {
        std::cerr << "Invalid mode: " << mode << std::endl;
        std::cerr << "Use 'server' or 'client'" << std::endl;
        return 1;
    }

    return 0;
}
