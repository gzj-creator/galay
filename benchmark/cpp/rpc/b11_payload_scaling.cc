#include <galay/cpp/galay-rpc/protoc/rpc_message.h>

#include <chrono>
#include <iostream>
#include <string>
#include <vector>

using namespace galay::rpc;

int main(int argc, char** argv)
{
    const size_t requests = argc > 1 ? static_cast<size_t>(std::stoull(argv[1])) : 10000;
    const size_t payload_size = argc > 2 ? static_cast<size_t>(std::stoull(argv[2])) : 1024;
    std::string payload(payload_size, 'x');
    size_t bytes = 0;
    size_t errors = 0;
    const auto start = std::chrono::steady_clock::now();
    for (size_t i = 0; i < requests; ++i) {
        RpcRequest request(static_cast<uint32_t>(i + 1), "BenchService", "echo");
        request.payload(payload.data(), payload.size());
        auto wire = request.serialize();
        auto decoded = RpcRequest{};
        if (!decoded.deserializeBody(wire.data() + RPC_HEADER_SIZE, wire.size() - RPC_HEADER_SIZE)) {
            ++errors;
        }
        bytes += wire.size();
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::cout << "RPC payload scaling\nrequests=" << requests
              << "\npayload=" << payload_size
              << "\nqps=" << (seconds > 0.0 ? static_cast<double>(requests) / seconds : 0.0)
              << "\nbytes_per_sec=" << (seconds > 0.0 ? static_cast<double>(bytes) / seconds : 0.0)
              << "\nerrors=" << errors << "\n";
    return errors == 0 ? 0 : 1;
}
