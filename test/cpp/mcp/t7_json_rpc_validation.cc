/**
 * @file t7_json_rpc_validation.cc
 * @brief 锁定 JSON-RPC 2.0 envelope 的请求/响应边界校验。
 */

#include <galay/cpp/galay-mcp/common/request_codec.h>

#include <iostream>
#include <string_view>

using galay::mcp::parse_json_rpc_request;
using galay::mcp::parse_json_rpc_response;

namespace {

bool require(bool condition, std::string_view message)
{
    if (!condition) {
        std::cerr << message << '\n';
        return false;
    }
    return true;
}

bool rejects_request(std::string_view body)
{
    return !parse_json_rpc_request(body).has_value();
}

bool rejects_response(std::string_view body)
{
    return !parse_json_rpc_response(body).has_value();
}

} // namespace

int main()
{
    if (!require(rejects_response(R"({"jsonrpc":"2.0","id":1,"result":{},"error":null})"),
                 "response with null error and result was accepted")) {
        return 1;
    }
    const auto null_response = parse_json_rpc_response(R"({"jsonrpc":"2.0","id":1,"result":null})");
    if (!require(null_response && null_response->response.hasResult && null_response->response.result.is_null(),
                 "response with null result was rejected")) {
        return 1;
    }
    if (!require(rejects_request(R"({"id":1,"method":"tools/list"})"),
                 "request without jsonrpc version was accepted")) {
        return 1;
    }
    if (!require(rejects_request(R"({"jsonrpc":"1.0","id":1,"method":"tools/list"})"),
                 "request with wrong jsonrpc version was accepted")) {
        return 1;
    }
    if (!require(rejects_request(R"({"jsonrpc":"2.0","id":null,"method":"tools/list"})"),
                 "request with null id was accepted")) {
        return 1;
    }
    if (!require(rejects_request(R"({"jsonrpc":"2.0","id":1.5,"method":"tools/list"})"),
                 "request with fractional id was accepted")) {
        return 1;
    }

    auto notification = parse_json_rpc_request(R"({"jsonrpc":"2.0","method":"notifications/initialized"})");
    if (!require(notification.has_value() && !notification->request.id.has_value(),
                 "valid notification without id was not parsed as a notification")) {
        return 1;
    }

    if (!require(rejects_response(R"({"id":1,"result":{}})"),
                 "response without jsonrpc version was accepted")) {
        return 1;
    }
    if (!require(rejects_response(R"({"jsonrpc":"2.0","id":1})"),
                 "response without result/error was accepted")) {
        return 1;
    }
    if (!require(rejects_response(R"({"jsonrpc":"2.0","id":1,"result":{},"error":{"code":-32603,"message":"x"}})"),
                 "response with both result and error was accepted")) {
        return 1;
    }
    if (!require(rejects_response(R"({"jsonrpc":"2.0","id":1,"error":{}})"),
                 "response with malformed error object was accepted")) {
        return 1;
    }

    auto response = parse_json_rpc_response(R"({"jsonrpc":"2.0","id":7,"result":{"ok":true}})");
    if (!require(response.has_value() && response->response.id == 7 && response->response.hasResult &&
                     !response->response.hasError,
                 "valid result response was rejected")) {
        return 1;
    }

    std::cout << "T7-JsonRpcValidation PASS\n";
    return 0;
}
