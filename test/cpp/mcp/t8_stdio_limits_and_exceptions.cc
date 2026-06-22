/**
 * @file t8_stdio_limits_and_exceptions.cc
 * @brief 覆盖 stdio line/response 限制以及 handler 异常不向客户端泄露。
 */

#include <galay/cpp/galay-mcp/server/stdio_server.h>
#include <galay/cpp/galay-mcp/common/mcp_policy.h>

#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

bool require(bool condition, std::string_view message)
{
    if (!condition) {
        std::cerr << message << '\n';
        return false;
    }
    return true;
}

std::string initializeRequest(int id = 1)
{
    return std::string(R"({"jsonrpc":"2.0","id":)") + std::to_string(id) +
           R"(,"method":"initialize","params":{"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"test","version":"1"}}})";
}

void bindStreams(galay::mcp::McpStdioServer& server, std::istream& input, std::ostream& output)
{
    server.setStreams(input, output);
}

} // namespace

int main()
{
    {
        galay::mcp::McpStdioServer server;
        galay::mcp::McpProductionPolicy policy;
        policy.transport.max_stdio_line_bytes = 64;
        server.setProductionPolicy(policy);
        std::string oversized(policy.transport.max_stdio_line_bytes + 1, 'x');
        std::istringstream input(oversized + "\n");
        std::ostringstream output;
        bindStreams(server, input, output);

        server.run();

        if (!require(output.str().find("Payload too large") != std::string::npos,
                     "oversized stdio line did not produce payload-too-large response")) {
            return 1;
        }
    }

    {
        galay::mcp::McpStdioServer server;
        galay::mcp::McpProductionPolicy policy;
        policy.transport.max_response_bytes = 512;
        server.setProductionPolicy(policy);
        server.addTool("boom", "throws", "{}", [](const galay::mcp::JsonElement&) {
            throw std::runtime_error("secret-token-should-not-leak");
            return std::expected<galay::mcp::JsonString, galay::mcp::McpError>{galay::mcp::JsonString("{}")};
        });

        std::istringstream input(
            initializeRequest(1) + "\n" +
            R"({"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"boom","arguments":{}}})" + "\n");
        std::ostringstream output;
        bindStreams(server, input, output);

        server.run();

        const std::string wire = output.str();
        if (!require(wire.find("Internal error") != std::string::npos,
                     "throwing handler did not return a generic internal error")) {
            return 1;
        }
        if (!require(wire.find("secret-token-should-not-leak") == std::string::npos,
                     "throwing handler leaked exception text to client")) {
            return 1;
        }
    }

    {
        galay::mcp::McpStdioServer server;
        galay::mcp::McpProductionPolicy policy;
        policy.transport.max_response_bytes = 256;
        server.setProductionPolicy(policy);
        server.addTool("large", "large response", "{}", [](const galay::mcp::JsonElement&) {
            return std::expected<galay::mcp::JsonString, galay::mcp::McpError>{
                galay::mcp::JsonString(1024, 'x')};
        });

        std::istringstream input(
            initializeRequest(3) + "\n" +
            R"({"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"large","arguments":{}}})" + "\n");
        std::ostringstream output;
        bindStreams(server, input, output);

        server.run();

        if (!require(output.str().find("Payload too large") != std::string::npos,
                     "oversized stdio response was not rejected before write")) {
            return 1;
        }
    }

    std::cout << "T8-StdioLimitsAndExceptions PASS\n";
    return 0;
}
