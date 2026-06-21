#include <galay/cpp/galay-rpc/kernel/rpc_connection_pool.h>
#include <galay/cpp/galay-kernel/core/runtime.h>

#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>

using namespace galay::kernel;
using namespace galay::rpc;

namespace {

struct Result {
    std::atomic<bool> done{false};
    size_t requests = 0;
    size_t errors = 0;
    double qps = 0.0;
};

Task<void> runPool(size_t requests, Result* result)
{
    RpcEndpoint endpoint{"127.0.0.1", 9000};
    RpcConnectionPoolConfig config;
    config.max_connections_per_endpoint = 1;
    config.max_waiters_per_endpoint = 1;
    RpcConnectionPool pool(config);
    pool.ensureEndpoint(endpoint);

    const auto start = std::chrono::steady_clock::now();
    for (size_t i = 0; i < requests; ++i) {
        auto acquired = co_await pool.acquire(endpoint);
        if (!acquired.has_value() || !acquired->has_value()) {
            ++result->errors;
            continue;
        }
        auto release = pool.release(std::move(acquired->value()));
        if (!release.has_value()) {
            ++result->errors;
        }
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    result->requests = requests;
    result->qps = seconds > 0.0 ? static_cast<double>(requests) / seconds : 0.0;
    result->done.store(true, std::memory_order_release);
    co_return;
}

} // namespace

int main(int argc, char** argv)
{
    const size_t requests = argc > 1 ? static_cast<size_t>(std::stoull(argv[1])) : 10000;
    Runtime runtime = RuntimeBuilder().ioSchedulerCount(1).computeSchedulerCount(0).build();
    runtime.start();
    Result result;
    auto scheduled = runtime.spawn(runPool(requests, &result));
    if (!scheduled.has_value()) {
        runtime.stop();
        std::cerr << "failed to schedule pool pressure\n";
        return 1;
    }
    while (!result.done.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    runtime.stop();
    std::cout << "RPC pool pressure\nrequests=" << result.requests
              << "\nqps=" << result.qps
              << "\nerrors=" << result.errors << "\n";
    return result.errors == 0 ? 0 : 1;
}
