#include "galay-mcp/client/client.h"
#include "galay-kernel/core/runtime.h"

#include <concepts>
#include <expected>
#include <string>
#include <utility>
#include <vector>

using galay::kernel::Runtime;
using galay::kernel::RuntimeBuilder;
using galay::mcp::JsonString;
using galay::mcp::McpClient;
using galay::mcp::McpError;
using galay::mcp::McpHttpClientConfig;
using galay::mcp::McpStdioClientConfig;
using galay::mcp::Prompt;
using galay::mcp::Resource;
using galay::mcp::Tool;

static_assert(requires(McpClient& client) {
    { client.mode() } -> std::same_as<galay::mcp::McpClientMode>;
    { client.isConnected() } -> std::same_as<bool>;
    { client.isInitialized() } -> std::same_as<bool>;
});

static_assert(!std::movable<McpClient>);

static_assert(requires(McpClient& client, const std::string& s, const JsonString& json) {
    { client.initialize(s, s) } -> std::same_as<std::expected<void, McpError>>;
    { client.callTool(s, json) } -> std::same_as<std::expected<JsonString, McpError>>;
    { client.listTools() } -> std::same_as<std::expected<std::vector<Tool>, McpError>>;
    { client.listResources() } -> std::same_as<std::expected<std::vector<Resource>, McpError>>;
    { client.readResource(s) } -> std::same_as<std::expected<std::string, McpError>>;
    { client.listPrompts() } -> std::same_as<std::expected<std::vector<Prompt>, McpError>>;
    { client.getPrompt(s, json) } -> std::same_as<std::expected<JsonString, McpError>>;
    { client.ping() } -> std::same_as<std::expected<void, McpError>>;
});

static_assert(requires(McpClient& client,
                       std::string s,
                       JsonString json,
                       std::expected<void, McpError>& void_result,
                       std::expected<JsonString, McpError>& json_result,
                       std::expected<std::vector<Tool>, McpError>& tools_result) {
    client.initialize(std::move(s), std::move(s), void_result);
    client.callTool(std::move(s), std::move(json), json_result);
    client.listTools(tools_result);
});

int main()
{
    McpClient stdio_client(McpStdioClientConfig{});

    Runtime runtime = RuntimeBuilder().ioSchedulerCount(1).computeSchedulerCount(1).build();
    McpClient http_client(runtime, McpHttpClientConfig{.url = "http://127.0.0.1:8080/mcp"});

    return stdio_client.mode() == galay::mcp::McpClientMode::Stdio &&
           http_client.mode() == galay::mcp::McpClientMode::Http
        ? 0
        : 1;
}
