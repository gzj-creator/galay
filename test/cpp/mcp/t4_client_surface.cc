#include <galay/cpp/galay-mcp/v1/client/client.h>
#include <galay/cpp/galay-kernel/core/runtime.h>

#include <concepts>
#include <expected>
#include <string>
#include <utility>
#include <vector>

using galay::kernel::Runtime;
using galay::kernel::RuntimeBuilder;
using galay::mcp::McpClient;
using galay::mcp::McpError;
using galay::mcp::McpHttpClientConfig;
using galay::mcp::McpStdioClientConfig;
using galay::mcp::Prompt;
using galay::mcp::Resource;
using galay::mcp::Tool;

static_assert(requires(McpClient& client) {
    { client.mode() } -> std::same_as<galay::mcp::McpClientMode>;
    { client.is_connected() } -> std::same_as<bool>;
    { client.is_initialized() } -> std::same_as<bool>;
});

static_assert(!std::movable<McpClient>);

static_assert(requires(McpClient& client, const std::string& s, const std::string& json) {
    { client.initialize(s, s) } -> std::same_as<std::expected<void, McpError>>;
    { client.call_tool(s, json) } -> std::same_as<std::expected<std::string, McpError>>;
    { client.list_tools() } -> std::same_as<std::expected<std::vector<Tool>, McpError>>;
    { client.list_resources() } -> std::same_as<std::expected<std::vector<Resource>, McpError>>;
    { client.read_resource(s) } -> std::same_as<std::expected<std::string, McpError>>;
    { client.list_prompts() } -> std::same_as<std::expected<std::vector<Prompt>, McpError>>;
    { client.get_prompt(s, json) } -> std::same_as<std::expected<std::string, McpError>>;
    { client.ping() } -> std::same_as<std::expected<void, McpError>>;
});

static_assert(requires(McpClient& client,
                       std::string s,
                       std::string json,
                       std::expected<void, McpError>& void_result,
                       std::expected<std::string, McpError>& json_result,
                       std::expected<std::vector<Tool>, McpError>& tools_result) {
    client.initialize(std::move(s), std::move(s), void_result);
    client.call_tool(std::move(s), std::move(json), json_result);
    client.list_tools(tools_result);
});

int main()
{
    McpClient stdio_client(McpStdioClientConfig{});

    Runtime runtime = RuntimeBuilder().io_scheduler_count(1).parallel_scheduler_count(1).build();
    McpClient http_client(runtime, McpHttpClientConfig{.url = "http://127.0.0.1:8080/mcp"});

    return stdio_client.mode() == galay::mcp::McpClientMode::Stdio &&
           http_client.mode() == galay::mcp::McpClientMode::Http
        ? 0
        : 1;
}
