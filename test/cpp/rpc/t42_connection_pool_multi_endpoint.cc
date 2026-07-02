#include <galay/cpp/galay-rpc/kernel/rpc_connection_pool.h>
#include <galay/cpp/galay-kernel/common/sleep.hpp>
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

struct TestState {
    std::atomic<bool> done{false};
    bool ok = true;
    std::string error;
};

void fail(TestState* state, std::string message)
{
    state->ok = false;
    state->error = std::move(message);
}

std::vector<RpcEndpoint> makeEndpoints(size_t count)
{
    std::vector<RpcEndpoint> endpoints;
    endpoints.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        endpoints.push_back(RpcEndpoint{"127.0.0.1", static_cast<uint16_t>(43000 + i)});
    }
    return endpoints;
}

Task<void> runMultiEndpointChecks(TestState* state)
{
    constexpr size_t kEndpointCount = 96;
    auto endpoints = makeEndpoints(kEndpointCount);

    RpcConnectionPoolConfig config;
    config.min_connections_per_endpoint = 1;
    config.max_connections_per_endpoint = 1;
    config.max_waiters_per_endpoint = 0;

    RpcConnectionPool pool(config);
    for (const auto& endpoint : endpoints) {
        auto ensure_result = pool.ensureEndpoint(endpoint);
        if (!ensure_result.has_value()) {
            fail(state, "ensureEndpoint rejected a valid endpoint");
            state->done.store(true, std::memory_order_release);
            co_return;
        }
    }

    std::vector<uint64_t> leased_ids;
    leased_ids.reserve(endpoints.size());
    std::vector<RpcPooledConnection> leases;
    leases.reserve(endpoints.size());

    for (const auto& endpoint : endpoints) {
        auto acquired = co_await pool.acquire(endpoint);
        if (!acquired.has_value() || !acquired->has_value()) {
            fail(state, "initial acquire failed for endpoint");
            state->done.store(true, std::memory_order_release);
            co_return;
        }
        leased_ids.push_back(acquired->value().id());
        leases.push_back(std::move(acquired->value()));
        if (pool.availableCount(endpoint) != 0 || pool.inUseCount(endpoint) != 1) {
            fail(state, "pool did not isolate per-endpoint in-use accounting");
            state->done.store(true, std::memory_order_release);
            co_return;
        }
    }

    for (const auto& endpoint : endpoints) {
        auto pressure = co_await pool.acquire(endpoint);
        if (!pressure.has_value() || pressure->has_value() ||
            pressure->error().code() != RpcErrorCode::RESOURCE_EXHAUSTED) {
            fail(state, "endpoint pressure did not return RESOURCE_EXHAUSTED");
            state->done.store(true, std::memory_order_release);
            co_return;
        }
    }

    for (auto& lease : leases) {
        auto release_result = pool.release(std::move(lease));
        if (!release_result.has_value()) {
            fail(state, "release failed after multi-endpoint pressure");
            state->done.store(true, std::memory_order_release);
            co_return;
        }
    }

    for (size_t i = 0; i < endpoints.size(); ++i) {
        auto reacquired = co_await pool.acquire(endpoints[i]);
        if (!reacquired.has_value() || !reacquired->has_value()) {
            fail(state, "reacquire failed after multi-endpoint release");
            state->done.store(true, std::memory_order_release);
            co_return;
        }
        if (reacquired->value().id() != leased_ids[i]) {
            fail(state, "endpoint lease was not returned to its original endpoint bucket");
            state->done.store(true, std::memory_order_release);
            co_return;
        }
        auto release_result = pool.release(std::move(reacquired->value()));
        if (!release_result.has_value()) {
            fail(state, "final release failed");
            state->done.store(true, std::memory_order_release);
            co_return;
        }
    }

    state->done.store(true, std::memory_order_release);
    co_return;
}

} // namespace

int main()
{
    Runtime runtime = RuntimeBuilder().ioSchedulerCount(1).computeSchedulerCount(0).build();
    runtime.start();

    TestState state;
    auto scheduled = runtime.spawn(runMultiEndpointChecks(&state));
    if (!scheduled.has_value()) {
        runtime.stop();
        std::cerr << "failed to schedule multi-endpoint pool checks\n";
        return 1;
    }

    for (int i = 0; i < 300 && !state.done.load(std::memory_order_acquire); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    runtime.stop();

    if (!state.done.load(std::memory_order_acquire)) {
        std::cerr << "multi-endpoint connection pool test timed out\n";
        return 1;
    }
    if (!state.ok) {
        std::cerr << state.error << "\n";
        return 1;
    }

    std::cout << "RPC connection pool multi-endpoint PASS\n";
    return 0;
}
