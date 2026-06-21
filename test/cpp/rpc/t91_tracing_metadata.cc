#include <galay/cpp/galay-rpc/kernel/rpc_tracing.h>

#include <iostream>

using namespace galay::rpc;

int main()
{
    RpcMetadata metadata;
    auto empty = setTraceparent(metadata, "");
    if (empty.has_value() || empty.error().code() != RpcErrorCode::INVALID_REQUEST) {
        std::cerr << "empty traceparent was accepted\n";
        return 1;
    }
    const std::string value = "00-0123456789abcdef0123456789abcdef-0123456789abcdef-01";
    auto set = setTraceparent(metadata, value);
    if (!set.has_value()) {
        std::cerr << "valid traceparent was rejected\n";
        return 1;
    }
    auto got = traceparent(metadata);
    if (!got.has_value() || *got != value) {
        std::cerr << "traceparent metadata did not round trip\n";
        return 1;
    }
    std::cout << "RPC tracing metadata PASS\n";
    return 0;
}
