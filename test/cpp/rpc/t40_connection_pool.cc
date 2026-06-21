#include <galay/cpp/galay-rpc/kernel/rpc_connection_pool.h>
#include <galay/cpp/galay-kernel/common/sleep.hpp>
#include <galay/cpp/galay-kernel/core/runtime.h>

#include <atomic>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>

using namespace galay::kernel;
using namespace galay::rpc;

namespace {

struct TestState {
    std::atomic<bool> done{false};
    std::atomic<bool> waiter_done{false};
    bool ok = true;
    std::string error;
    uint64_t waiter_connection_id = 0;
};

void fail(TestState* state, std::string message)
{
    state->ok = false;
    state->error = std::move(message);
}

Task<void> runPoolChecks(TestState* state)
{
    RpcEndpoint endpoint{"127.0.0.1", 41000};

    RpcConnectionPoolConfig config;
    config.min_connections_per_endpoint = 1;
    config.max_connections_per_endpoint = 1;
    config.max_waiters_per_endpoint = 1;

    RpcConnectionPool pool(config);
    auto ensure_result = pool.ensureEndpoint(endpoint);
    if (!ensure_result.has_value()) {
        fail(state, "ensureEndpoint rejected a valid endpoint");
        state->done.store(true, std::memory_order_release);
        co_return;
    }

    auto first = co_await pool.acquire(endpoint);
    if (!first.has_value() || !first->has_value()) {
        fail(state, "first acquire failed");
        state->done.store(true, std::memory_order_release);
        co_return;
    }
    RpcPooledConnection first_connection = std::move(first->value());
    const uint64_t first_id = first_connection.id();
    if (pool.availableCount(endpoint) != 0 || pool.inUseCount(endpoint) != 1) {
        fail(state, "pool did not track checked-out connection");
        state->done.store(true, std::memory_order_release);
        co_return;
    }

    auto runtime = RuntimeHandle::current();
    if (!runtime.has_value()) {
        fail(state, "runtime handle unavailable");
        state->done.store(true, std::memory_order_release);
        co_return;
    }
    auto waiter_task = runtime->spawn([](RpcConnectionPool* pool_ptr,
                                         RpcEndpoint waiter_endpoint,
                                         TestState* waiter_state) -> Task<void> {
        auto waited = co_await pool_ptr->acquire(waiter_endpoint);
        if (waited.has_value() && waited->has_value()) {
            waiter_state->waiter_connection_id = waited->value().id();
            auto release_result = pool_ptr->release(std::move(waited->value()));
            if (!release_result.has_value()) {
                fail(waiter_state, "waiter release failed");
            }
        } else {
            fail(waiter_state, "waiter acquire failed");
        }
        waiter_state->waiter_done.store(true, std::memory_order_release);
        co_return;
    }(&pool, endpoint, state));
    if (!waiter_task.has_value()) {
        fail(state, "failed to schedule pool waiter");
        state->done.store(true, std::memory_order_release);
        co_return;
    }
    while (pool.waiterCount(endpoint) == 0) {
        co_await sleep(std::chrono::milliseconds(1));
    }

    auto pressure = co_await pool.acquire(endpoint);
    if (!pressure.has_value() || pressure->has_value() ||
        pressure->error().code() != RpcErrorCode::RESOURCE_EXHAUSTED) {
        fail(state, "pool pressure did not return RESOURCE_EXHAUSTED");
        state->done.store(true, std::memory_order_release);
        co_return;
    }

    auto release_result = pool.release(std::move(first_connection));
    if (!release_result.has_value()) {
        fail(state, "release failed");
        state->done.store(true, std::memory_order_release);
        co_return;
    }

    while (!state->waiter_done.load(std::memory_order_acquire)) {
        co_await sleep(std::chrono::milliseconds(1));
    }
    if (!state->ok || state->waiter_connection_id != first_id) {
        fail(state, "waiter did not receive the released connection");
        state->done.store(true, std::memory_order_release);
        co_return;
    }

    auto replacement = co_await pool.acquire(endpoint);
    if (!replacement.has_value() || !replacement->has_value()) {
        fail(state, "replacement acquire failed");
        state->done.store(true, std::memory_order_release);
        co_return;
    }
    RpcPooledConnection replacement_connection = std::move(replacement->value());
    replacement_connection.markBroken();
    auto broken_release = pool.release(std::move(replacement_connection));
    if (!broken_release.has_value()) {
        fail(state, "broken release failed");
        state->done.store(true, std::memory_order_release);
        co_return;
    }

    auto after_broken = co_await pool.acquire(endpoint);
    if (!after_broken.has_value() || !after_broken->has_value()) {
        fail(state, "replacement acquire failed after broken release");
        state->done.store(true, std::memory_order_release);
        co_return;
    }
    RpcPooledConnection after_broken_connection = std::move(after_broken->value());
    if (after_broken_connection.id() == first_id) {
        fail(state, "broken connection was reused instead of replaced");
        state->done.store(true, std::memory_order_release);
        co_return;
    }

    auto blocked = pool.acquire(endpoint);
    auto shutdown_result = pool.shutdown();
    if (!shutdown_result.has_value()) {
        fail(state, "shutdown failed");
        state->done.store(true, std::memory_order_release);
        co_return;
    }

    auto blocked_result = co_await blocked;
    if (!blocked_result.has_value() || blocked_result->has_value() ||
        blocked_result->error().code() != RpcErrorCode::CONNECTION_CLOSED) {
        fail(state, "shutdown did not wake waiter with CONNECTION_CLOSED");
        state->done.store(true, std::memory_order_release);
        co_return;
    }
    if (pool.totalTrackedConnections(endpoint) != 0) {
        fail(state, "shutdown did not clear tracked connections");
        state->done.store(true, std::memory_order_release);
        co_return;
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
    auto scheduled = runtime.spawn(runPoolChecks(&state));
    if (!scheduled.has_value()) {
        runtime.stop();
        std::cerr << "failed to schedule pool checks\n";
        return 1;
    }

    for (int i = 0; i < 300 && !state.done.load(std::memory_order_acquire); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    runtime.stop();

    if (!state.done.load(std::memory_order_acquire)) {
        std::cerr << "connection pool test timed out\n";
        return 1;
    }
    if (!state.ok) {
        std::cerr << state.error << "\n";
        return 1;
    }

    std::cout << "RPC connection pool PASS\n";
    return 0;
}
