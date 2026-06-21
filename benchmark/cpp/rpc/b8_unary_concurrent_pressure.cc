#include <galay/cpp/galay-rpc/kernel/rpc_channel.h>

#include <chrono>
#include <iostream>

using namespace galay::rpc;

int main(int argc, char** argv)
{
    size_t requests = argc > 1 ? static_cast<size_t>(std::stoull(argv[1])) : 100000;
    RpcChannelOptions options;
    options.max_in_flight = requests + 1;
    RpcChannelState state(options);
    const auto start = std::chrono::steady_clock::now();
    size_t errors = 0;
    for (size_t i = 0; i < requests; ++i) {
        auto pending = state.registerPending(static_cast<uint32_t>(i + 1));
        if (!pending.has_value()) {
            ++errors;
            continue;
        }
        state.failPending(static_cast<uint32_t>(i + 1), RpcError(RpcErrorCode::CANCELLED, "done"));
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::cout << "RPC unary concurrent pressure\nrequests=" << requests
              << "\nqps=" << (seconds > 0 ? requests / seconds : 0)
              << "\nerrors=" << errors << "\n";
    return errors == 0 ? 0 : 1;
}
