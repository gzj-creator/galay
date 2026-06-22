/**
 * @file t7_json_rpc_validation.cc
 * @brief 锁定 JSON-RPC 2.0 envelope 的请求/响应边界校验。
 */

#include <galay/cpp/galay-mcp/common/json_parser.h>

#include <iostream>
#include <string_view>

using galay::mcp::parseJsonRpcRequest;
using galay::mcp::parseJsonRpcResponse;

namespace {

bool require(bool condition, std::string_view message)
{
    if (!condition) {
        std::cerr << message << '\n';
        return false;
    }
    return true;
}

bool rejectsRequest(std::string_view body)
{
    return !parseJsonRpcRequest(body).has_value();
}

bool rejectsResponse(std::string_view body)
{
    return !parseJsonRpcResponse(body).has_value();
}

} // namespace

int main()
{
    if (!require(rejectsRequest(R"({"id":1,"method":"tools/list"})"),
                 "request without jsonrpc version was accepted")) {
        return 1;
    }
    if (!require(rejectsRequest(R"({"jsonrpc":"1.0","id":1,"method":"tools/list"})"),
                 "request with wrong jsonrpc version was accepted")) {
        return 1;
    }
    if (!require(rejectsRequest(R"({"jsonrpc":"2.0","id":null,"method":"tools/list"})"),
                 "request with null id was accepted")) {
        return 1;
    }
    if (!require(rejectsRequest(R"({"jsonrpc":"2.0","id":1.5,"method":"tools/list"})"),
                 "request with fractional id was accepted")) {
        return 1;
    }

    auto notification = parseJsonRpcRequest(R"({"jsonrpc":"2.0","method":"notifications/initialized"})");
    if (!require(notification.has_value() && !notification->request.id.has_value(),
                 "valid notification without id was not parsed as a notification")) {
        return 1;
    }

    if (!require(rejectsResponse(R"({"id":1,"result":{}})"),
                 "response without jsonrpc version was accepted")) {
        return 1;
    }
    if (!require(rejectsResponse(R"({"jsonrpc":"2.0","id":1})"),
                 "response without result/error was accepted")) {
        return 1;
    }
    if (!require(rejectsResponse(R"({"jsonrpc":"2.0","id":1,"result":{},"error":{"code":-32603,"message":"x"}})"),
                 "response with both result and error was accepted")) {
        return 1;
    }
    if (!require(rejectsResponse(R"({"jsonrpc":"2.0","id":1,"error":{}})"),
                 "response with malformed error object was accepted")) {
        return 1;
    }

    auto response = parseJsonRpcResponse(R"({"jsonrpc":"2.0","id":7,"result":{"ok":true}})");
    if (!require(response.has_value() && response->response.id == 7 && response->response.hasResult &&
                     !response->response.hasError,
                 "valid result response was rejected")) {
        return 1;
    }

    std::cout << "T7-JsonRpcValidation PASS\n";
    return 0;
}
