#include "galay-mcp/server/stdio_server.h"
#include "galay-mcp/common/schema_builder.h"
#include <iostream>
#include <string>

using namespace galay::mcp;

int main() {
    McpStdioServer server;

    // 设置服务器信息
    server.setServerInfo("test-mcp-server", "1.0.0");

    // 添加一个简单的加法工具
    auto addSchema = SchemaBuilder()
        .addNumber("a", "First number", true)
        .addNumber("b", "Second number", true)
        .build();

    server.addTool("add", "Add two numbers", addSchema,
        [](const JsonElement& args) -> std::expected<JsonString, McpError> {
            JsonObject obj;
            if (!JsonHelper::GetObject(args, obj)) {
                return std::unexpected(McpError::toolExecutionFailed("Invalid arguments"));
            }
            auto aVal = obj["a"];
            auto bVal = obj["b"];
            if (aVal.error() || bVal.error()) {
                return std::unexpected(McpError::toolExecutionFailed("Missing parameters"));
            }
            double a = aVal.is_double() ? aVal.get_double().value() : static_cast<double>(aVal.get_int64().value());
            double b = bVal.is_double() ? bVal.get_double().value() : static_cast<double>(bVal.get_int64().value());

            JsonWriter writer;
            writer.StartObject();
            writer.Key("result");
            writer.Number(a + b);
            writer.EndObject();
            return writer.TakeString();
        }
    );

    // 添加一个字符串连接工具
    auto concatSchema = SchemaBuilder()
        .addString("str1", "First string", true)
        .addString("str2", "Second string", true)
        .build();

    server.addTool("concat", "Concatenate two strings", concatSchema,
        [](const JsonElement& args) -> std::expected<JsonString, McpError> {
            JsonObject obj;
            if (!JsonHelper::GetObject(args, obj)) {
                return std::unexpected(McpError::toolExecutionFailed("Invalid arguments"));
            }
            std::string str1;
            std::string str2;
            if (!JsonHelper::GetString(obj, "str1", str1) || !JsonHelper::GetString(obj, "str2", str2)) {
                return std::unexpected(McpError::toolExecutionFailed("Missing parameters"));
            }

            JsonWriter writer;
            writer.StartObject();
            writer.Key("result");
            writer.String(str1 + str2);
            writer.EndObject();
            return writer.TakeString();
        }
    );

    // 添加一个简单的资源
    server.addResource("file:///test.txt", "test.txt", "Test file", "text/plain",
        [](const std::string& uri) -> std::expected<std::string, McpError> {
            if (uri == "file:///test.txt") {
                return "This is a test file content.";
            }
            return std::unexpected(McpError::resourceNotFound(uri));
        }
    );

    // 添加一个提示
    auto promptArgs = PromptArgumentBuilder()
        .addArgument("topic", "The topic to write about", true)
        .build();

    server.addPrompt("write_essay", "Generate an essay prompt", promptArgs,
        [](const std::string& name, const JsonElement& args) -> std::expected<JsonString, McpError> {
            JsonObject obj;
            if (!JsonHelper::GetObject(args, obj)) {
                return std::unexpected(McpError::internalError("Invalid arguments"));
            }
            std::string topic;
            if (!JsonHelper::GetString(obj, "topic", topic)) {
                return std::unexpected(McpError::internalError("Missing topic"));
            }

            JsonWriter writer;
            writer.StartObject();
            writer.Key("description");
            writer.String("Essay prompt");
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
            writer.String("Write an essay about: " + topic);
            writer.EndObject();
            writer.EndObject();
            writer.EndArray();
            writer.EndObject();
            return writer.TakeString();
        }
    );

    // 运行服务器
    std::cerr << "MCP Server started. Waiting for requests..." << std::endl;
    server.run();
    std::cerr << "MCP Server stopped." << std::endl;

    return 0;
}
