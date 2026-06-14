/**
 * @file T4-HttpServer.cc
 * @brief HTTP MCP Server 测试示例
 */

#include "mcp/server/http_server.h"
#include "mcp/common/schema_builder.h"
#include <iostream>
#include <string>
#include <signal.h>
#include <thread>

using namespace galay::mcp;
using namespace galay::kernel;

McpHttpServer* g_server = nullptr;

void signalHandler(int signum) {
    std::cout << "\nReceived signal " << signum << ", stopping server...\n";
    if (g_server) {
        g_server->stop();
    }
}

// Echo工具（协程）
Coroutine echoTool(const JsonElement& arguments, std::expected<JsonString, McpError>& result) {
    JsonObject obj;
    if (!JsonHelper::GetObject(arguments, obj)) {
        result = std::unexpected(McpError::invalidParams("Invalid arguments"));
        co_return;
    }

    std::string message;
    JsonHelper::GetString(obj, "message", message);

    JsonWriter writer;
    writer.StartObject();
    writer.Key("echo");
    writer.String(message);
    writer.Key("length");
    writer.Number(static_cast<int64_t>(message.length()));
    writer.EndObject();
    result = writer.TakeString();
    co_return;
}

// 加法工具（协程）
Coroutine addTool(const JsonElement& arguments, std::expected<JsonString, McpError>& result) {
    JsonObject obj;
    if (!JsonHelper::GetObject(arguments, obj)) {
        result = std::unexpected(McpError::invalidParams("Invalid arguments"));
        co_return;
    }

    auto aVal = obj["a"];
    auto bVal = obj["b"];
    if (aVal.error() || bVal.error()) {
        result = std::unexpected(McpError::invalidParams("Missing parameters"));
        co_return;
    }

    double a = 0.0;
    double b = 0.0;
    if (aVal.is_double()) {
        a = aVal.get_double().value();
    } else if (aVal.is_int64()) {
        a = static_cast<double>(aVal.get_int64().value());
    } else {
        result = std::unexpected(McpError::invalidParams("Invalid parameter 'a'"));
        co_return;
    }

    if (bVal.is_double()) {
        b = bVal.get_double().value();
    } else if (bVal.is_int64()) {
        b = static_cast<double>(bVal.get_int64().value());
    } else {
        result = std::unexpected(McpError::invalidParams("Invalid parameter 'b'"));
        co_return;
    }

    JsonWriter writer;
    writer.StartObject();
    writer.Key("sum");
    writer.Number(a + b);
    writer.EndObject();
    result = writer.TakeString();
    co_return;
}

// 资源读取器（协程）
Coroutine readExampleResource(const std::string& uri, std::expected<std::string, McpError>& result) {
    if (uri == "example://hello") {
        result = "Hello from MCP HTTP Server!";
    } else if (uri == "example://info") {
        result = "This is a test resource from the HTTP MCP server.";
    } else {
        result = std::unexpected(McpError::resourceNotFound(uri));
    }
    co_return;
}

// 提示获取器（协程）
Coroutine getExamplePrompt(const std::string& name, const JsonElement& arguments, std::expected<JsonString, McpError>& result) {
    if (name == "greeting") {
        std::string userName = "User";
        JsonObject obj;
        if (JsonHelper::GetObject(arguments, obj)) {
            JsonHelper::GetString(obj, "name", userName);
        }

        JsonWriter writer;
        writer.StartObject();
        writer.Key("description");
        writer.String("A friendly greeting");
        writer.Key("messages");
        writer.StartArray();
        writer.StartObject();
        writer.Key("role");
        writer.String("user");
        writer.Key("content");
        writer.StartObject();
        writer.Key("type");
        writer.String("text");
        writer.Key("text");
        writer.String("Hello, " + userName + "! How can I help you today?");
        writer.EndObject();
        writer.EndObject();
        writer.EndArray();
        writer.EndObject();
        result = writer.TakeString();
    } else {
        result = std::unexpected(McpError::promptNotFound(name));
    }
    co_return;
}

int main(int argc, char* argv[]) {
    std::string host = "0.0.0.0";
    int port = 8080;

    if (argc > 1) {
        port = std::atoi(argv[1]);
    }
    if (argc > 2) {
        host = argv[2];
    }

    std::cout << "========================================\n";
    std::cout << "HTTP MCP Server Test\n";
    std::cout << "========================================\n";
    std::cout << "Server will listen on " << host << ":" << port << "\n";
    std::cout << "MCP endpoint: http://" << host << ":" << port << "/mcp\n";
    std::cout << "========================================\n\n";

    try {
        const unsigned int hardware_threads = std::thread::hardware_concurrency();
        const size_t io_schedulers = hardware_threads == 0 ? 8U : static_cast<size_t>(hardware_threads);

        McpHttpServer server(host, port, io_schedulers, 0);
        g_server = &server;

        signal(SIGINT, signalHandler);
        signal(SIGTERM, signalHandler);

        server.setServerInfo("test-http-mcp-server", "1.0.0");

        auto echoSchema = SchemaBuilder()
            .addString("message", "The message to echo", true)
            .build();
        server.addTool("echo", "Echo back the input message", echoSchema, echoTool);

        auto addSchema = SchemaBuilder()
            .addNumber("a", "First number", true)
            .addNumber("b", "Second number", true)
            .build();
        server.addTool("add", "Add two numbers", addSchema, addTool);

        server.addResource("example://hello", "Hello Resource",
                          "A simple hello message", "text/plain",
                          readExampleResource);

        server.addResource("example://info", "Info Resource",
                          "Information about the server", "text/plain",
                          readExampleResource);

        auto promptArgs = PromptArgumentBuilder()
            .addArgument("name", "User's name", false)
            .build();
        server.addPrompt("greeting", "Generate a friendly greeting",
                        promptArgs, getExamplePrompt);

        std::cout << "Server configured with:\n";
        std::cout << "  - IO schedulers: " << io_schedulers << "\n";
        std::cout << "  - Tools: echo, add\n";
        std::cout << "  - Resources: example://hello, example://info\n";
        std::cout << "  - Prompts: greeting\n";
        std::cout << "========================================\n";
        std::cout << "Starting server...\n";
        std::cout << "Press Ctrl+C to stop\n";
        std::cout << "========================================\n\n";

        server.start();

        std::cout << "\nServer stopped.\n";

    } catch (const std::exception& e) {
        std::cerr << "Server error: " << e.what() << "\n";
        return 1;
    }

    return 0;
}
