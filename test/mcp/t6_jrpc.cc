/**
 * @file t6_jrpc.cc
 * @brief 锁定 JSON-RPC HTTP 请求体快速构造 helper 的输出格式，避免热路径优化改坏协议序列化。
 */

#include "mcp/common/protocol_utils.h"

#include <iostream>
#include <optional>
#include <string_view>

using galay::mcp::protocol::makeJsonRpcRequestBody;

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
    const auto no_params = makeJsonRpcRequestBody(7, "tools/list", std::nullopt);
    if (!require(no_params == R"({"jsonrpc":"2.0","id":7,"method":"tools/list"})",
                 "unexpected JSON-RPC body for request without params")) {
        return 1;
    }

    const auto with_params =
        makeJsonRpcRequestBody(9, "tools/call", std::optional<std::string_view>(R"({"name":"echo"})"));
    if (!require(with_params == R"({"jsonrpc":"2.0","id":9,"method":"tools/call","params":{"name":"echo"}})",
                 "unexpected JSON-RPC body for request with params")) {
        return 1;
    }

    std::cout << "T6-JsonRpcRequestBodySurface PASS\n";
    return 0;
}
