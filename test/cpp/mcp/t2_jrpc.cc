/**
 * @file t6_jrpc.cc
 * @brief 锁定 JSON-RPC HTTP 请求体快速构造 helper 的输出格式，避免热路径优化改坏协议序列化。
 */

#include <galay/cpp/galay-mcp/common/protocol_utils.h>

#include <iostream>
#include <optional>
#include <string_view>

using galay::mcp::protocol::make_json_rpc_request_body;

namespace {

bool require(bool condition, std::string_view message)
{
    if (!condition) {
        std::cerr << message << '\n';
        return false;
    }
    return true;
}

} // namespace

int main()
{
    const auto no_params = make_json_rpc_request_body(7, "tools/list", std::nullopt);
    const auto parsed_no_params = json::deserialize<galay::mcp::JsonRpcRequest>(no_params);
    if (!require(parsed_no_params && parsed_no_params->id == 7 &&
                     parsed_no_params->method == "tools/list" && !parsed_no_params->params,
                 "unexpected JSON-RPC body for request without params")) {
        return 1;
    }

    const auto with_params =
        make_json_rpc_request_body(9, "tools/call", std::optional<std::string_view>(R"({"name":"echo"})"));
    const auto parsed_with_params = json::deserialize<galay::mcp::JsonRpcRequest>(with_params);
    if (!require(parsed_with_params && parsed_with_params->id == 9 &&
                     parsed_with_params->method == "tools/call" && parsed_with_params->params == R"({"name":"echo"})",
                 "unexpected JSON-RPC body for request with params")) {
        return 1;
    }

    std::cout << "T6-JsonRpcRequestBodySurface PASS\n";
    return 0;
}
