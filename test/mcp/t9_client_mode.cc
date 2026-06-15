#include "galay-mcp/client/client.h"
#include "galay-mcp/common/mcp_error.h"
#include "galay-kernel/core/runtime.h"

#include <expected>
#include <iostream>
#include <vector>

using galay::kernel::Runtime;
using galay::kernel::RuntimeBuilder;
using galay::mcp::McpClient;
using galay::mcp::McpErrorCode;
using galay::mcp::McpHttpClientConfig;
using galay::mcp::McpStdioClientConfig;

int main()
{
    Runtime runtime = RuntimeBuilder().ioSchedulerCount(1).computeSchedulerCount(1).build();
    McpClient http_client(runtime, McpHttpClientConfig{.url = "http://127.0.0.1:8080/mcp"});
    auto wrong_sync = http_client.listTools();
    if (wrong_sync || wrong_sync.error().code() != McpErrorCode::InvalidTransportMode) {
        std::cerr << "HTTP client accepted stdio sync API\n";
        return 1;
    }
    auto wrong_disconnect = http_client.disconnect();
    if (wrong_disconnect || wrong_disconnect.error().code() != McpErrorCode::InvalidTransportMode) {
        std::cerr << "HTTP client accepted stdio disconnect API\n";
        return 1;
    }

    McpClient stdio_client(McpStdioClientConfig{});
    runtime.start();
    std::expected<void, galay::mcp::McpError> wrong_async_result;
    auto join = runtime.spawn(stdio_client.ping(wrong_async_result));
    if (!join) {
        runtime.stop();
        std::cerr << "failed to spawn wrong-mode async task\n";
        return 1;
    }
    auto join_result = join->join();
    runtime.stop();
    if (!join_result) {
        std::cerr << "wrong-mode async task join failed\n";
        return 1;
    }
    if (wrong_async_result || wrong_async_result.error().code() != McpErrorCode::InvalidTransportMode) {
        std::cerr << "stdio client accepted HTTP async API\n";
        return 1;
    }

    if (stdio_client.mode() != galay::mcp::McpClientMode::Stdio) {
        std::cerr << "stdio mode not recorded\n";
        return 1;
    }
    if (http_client.mode() != galay::mcp::McpClientMode::Http) {
        std::cerr << "http mode not recorded\n";
        return 1;
    }

    McpClient null_input_client(McpStdioClientConfig{.input = nullptr, .output = &std::cout});
    auto null_input_result = null_input_client.initialize("null-input", "1.0.0");
    if (null_input_result ||
        null_input_result.error().code() != McpErrorCode::InvalidParams) {
        std::cerr << "stdio client did not reject null input stream\n";
        return 1;
    }

    McpClient null_output_client(McpStdioClientConfig{.input = &std::cin, .output = nullptr});
    auto null_output_result = null_output_client.initialize("null-output", "1.0.0");
    if (null_output_result ||
        null_output_result.error().code() != McpErrorCode::InvalidParams) {
        std::cerr << "stdio client did not reject null output stream\n";
        return 1;
    }

    return 0;
}
