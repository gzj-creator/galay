#include <galay/cpp/galay-rpc/kernel/rpc_connection_pool.h>
#include <galay/cpp/galay-kernel/core/runtime.h>

#include <atomic>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

using namespace galay::kernel;
using namespace galay::rpc;

namespace {

struct Result {
    std::atomic<bool> done{false};
    size_t operations = 0;
    size_t endpoints = 0;
    size_t errors = 0;
    double qps = 0.0;
};

std::vector<RpcEndpoint> makeEndpoints(size_t count)
{
    std::vector<RpcEndpoint> endpoints;
    endpoints.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        endpoints.push_back(RpcEndpoint{"127.0.0.1", static_cast<uint16_t>(45000 + i)});
    }
    return endpoints;
}

Task<void> runEndpointSwitching(size_t operations, size_t endpoint_count, Result* result)
{
    RpcConnectionPoolConfig config;
    config.max_connections_per_endpoint = 1;
    config.max_waiters_per_endpoint = 0;

    RpcConnectionPool pool(config);
    auto endpoints = makeEndpoints(endpoint_count);
    for (const auto& endpoint : endpoints) {
        auto ensure_result = pool.ensureEndpoint(endpoint);
        if (!ensure_result.has_value()) {
            ++result->errors;
            result->done.store(true, std::memory_order_release);
            co_return;
        }
    }

    const auto start = std::chrono::steady_clock::now();
    for (size_t i = 0; i < operations; ++i) {
        const auto& endpoint = endpoints[i % endpoints.size()];
        auto acquired = co_await pool.acquire(endpoint);
        if (!acquired.has_value() || !acquired->has_value()) {
            ++result->errors;
            continue;
        }
        auto release_result = pool.release(std::move(acquired->value()));
        if (!release_result.has_value()) {
            ++result->errors;
        }
    }
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    result->operations = operations;
    result->endpoints = endpoints.size();
    result->qps = seconds > 0.0 ? static_cast<double>(operations) / seconds : 0.0;
    result->done.store(true, std::memory_order_release);
    co_return;
}

} // namespace

int main(int argc, char** argv)
{
    const size_t operations = argc > 1 ? static_cast<size_t>(std::stoull(argv[1])) : 200000;
    const size_t endpoint_count = argc > 2 ? static_cast<size_t>(std::stoull(argv[2])) : 64;
    if (endpoint_count == 0 || endpoint_count > 512) {
        std::cerr << "endpoint_count must be in [1, 512]\n";
        return 1;
    }

    Runtime runtime = RuntimeBuilder().ioSchedulerCount(1).computeSchedulerCount(0).build();
    runtime.start();

    Result result;
    auto scheduled = runtime.spawn(runEndpointSwitching(operations, endpoint_count, &result));
    if (!scheduled.has_value()) {
        runtime.stop();
        std::cerr << "failed to schedule endpoint-switching pool pressure\n";
        return 1;
    }
    while (!result.done.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    runtime.stop();

    std::cout << "RPC pool endpoint switching\noperations=" << result.operations
              << "\nendpoints=" << result.endpoints
              << "\nqps=" << result.qps
              << "\nerrors=" << result.errors << "\n";
    return result.errors == 0 ? 0 : 1;
}
