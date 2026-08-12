/**
 * @file t16_v2_stdio_server.cc
 * @brief MCP 2026-07-28 无初始化 stdio 服务端端到端边界。
 */

#include <galay/cpp/galay-mcp/v2/stdio_server.h>

#include <iostream>
#include <sstream>
#include <string_view>

using namespace galay::mcp;

namespace {

bool require(bool value, std::string_view message)
{
    if (!value) {
        std::cerr << message << '\n';
    }
    return value;
}

std::string request(int id, std::string_view method, std::string_view fields = "{}")
{
    v2::RequestMeta meta;
    meta.clientInfo = v2::Implementation{.name = "t16", .version = "1"};
    auto params = v2::makeRequestParams(meta, fields);
    if (!params) {
        return {};
    }
    v2::JsonRpcRequest message;
    message.id = id;
    message.method = std::string(method);
    message.params = std::move(params.value());
    return message.toJson();
}

} // namespace

int main()
{
    std::istringstream input(
        request(1, v2::Methods::SERVER_DISCOVER) + '\n' +
        request(2, v2::Methods::TOOLS_LIST) + '\n' +
        request(3, v2::Methods::TOOLS_CALL, R"({"name":"echo","arguments":{"text":"hi"}})") + '\n' +
        request(4, v2::Methods::RESOURCES_READ, R"({"uri":"mem://hello"})") + '\n' +
        request(5, v2::Methods::PROMPTS_GET, R"({"name":"review","arguments":{}})") + '\n' +
        request(6, v2::Methods::TOOLS_LIST) + '\n');
    std::ostringstream output;

    v2::McpStdioServer server;
    server.setServerInfo("t16-server", "2.0.0");
    server.addTool("echo", "Echo text", R"({"type":"object"})",
                   [](const JsonElement&) -> std::expected<JsonString, McpError> {
                       return JsonString("hello");
                   });
    server.addResource("mem://hello", "hello", "Hello", "text/plain",
                       [](const std::string&) -> std::expected<std::string, McpError> {
                           return std::string("hello");
                       });
    server.addPrompt("review", "Review", {},
                     [](const std::string&, const JsonElement&) -> std::expected<JsonString, McpError> {
                         return JsonString(R"({"messages":[{"role":"user","content":{"type":"text","text":"Review"}}]})");
                     });
    server.setStreams(input, output);
    server.run();

    std::string line;
    std::istringstream responses(output.str());
    std::size_t count = 0;
    bool sawDiscover = false;
    bool sawTools = false;
    bool sawCall = false;
    bool sawRead = false;
    bool sawPrompt = false;
    while (std::getline(responses, line)) {
        ++count;
        auto parsed = v2::parseResponse(line);
        if (!require(parsed.has_value(), "v2 stdio response was invalid")) {
            return 1;
        }
        if (!parsed->response.hasResult) {
            continue;
        }
        JsonObject result;
        if (!require(JsonHelper::getObject(parsed->response.result, result),
                     "v2 stdio result was not an object")) {
            return 1;
        }
        std::string type;
        if (!require(JsonHelper::getString(result, "resultType", type) && type == "complete",
                     "v2 stdio resultType was missing")) {
            return 1;
        }
        int64_t id = std::get<int64_t>(parsed->response.id);
        sawDiscover |= id == 1 && result["supportedVersions"].error() == simdjson::SUCCESS;
        sawTools |= id == 2 && result["tools"].error() == simdjson::SUCCESS;
        sawCall |= id == 3 && result["content"].error() == simdjson::SUCCESS;
        sawRead |= id == 4 && result["contents"].error() == simdjson::SUCCESS;
        sawPrompt |= id == 5 && result["messages"].error() == simdjson::SUCCESS;
        if (id == 6) {
            if (!require(result["tools"].error() == simdjson::SUCCESS,
                         "second stateless v2 request failed")) {
                return 1;
            }
        }
    }

    if (!require(count == 6, "v2 stdio server did not answer every request") ||
        !require(sawDiscover && sawTools && sawCall && sawRead && sawPrompt,
                 "v2 stdio server missed one protocol method")) {
        return 1;
    }

    std::cout << "T16-V2StdioServer PASS\n";
    return 0;
}
