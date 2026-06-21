#include <galay/cpp/galay-rpc/kernel/rpc_managed_client.h>

#include <chrono>
#include <iostream>

using namespace galay::rpc;

int main(int argc, char** argv)
{
    const size_t requests = argc > 1 ? static_cast<size_t>(std::stoull(argv[1])) : 100000;
    RpcStaticDiscovery discovery;
    discovery.set("BenchService", {
        RpcEndpoint{"127.0.0.1", 9001},
        RpcEndpoint{"127.0.0.1", 9002},
        RpcEndpoint{"127.0.0.1", 9003},
    });
    RpcManagedClient client(discovery);
    auto refresh = client.refresh("BenchService");
    if (!refresh.has_value()) {
        std::cerr << "refresh failed\n";
        return 1;
    }
    size_t errors = 0;
    const auto start = std::chrono::steady_clock::now();
    for (size_t i = 0; i < requests; ++i) {
        auto endpoint = client.selectEndpoint("BenchService");
        if (!endpoint.has_value()) {
            ++errors;
        }
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::cout << "RPC managed client pressure\nrequests=" << requests
              << "\nqps=" << (seconds > 0.0 ? static_cast<double>(requests) / seconds : 0.0)
              << "\nerrors=" << errors << "\n";
    return errors == 0 ? 0 : 1;
}
