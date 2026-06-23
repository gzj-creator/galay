#include <galay/cpp/galay-redis/async/conn_pool.h>
#include <galay/cpp/galay-redis/async/redis_client.h>
#include <galay/cpp/galay-kernel/common/sleep.hpp>
#include <galay/cpp/galay-kernel/core/runtime.h>

#include "integration_config.h"

#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

using namespace galay::kernel;
using namespace galay::redis;
using namespace std::chrono_literals;

namespace {

struct TestState {
    std::atomic<bool> done{false};
    bool ok = true;
    std::string error;
};

struct WaiterProbe {
    std::atomic<bool> done{false};
    bool got_connection = false;
    bool got_timeout = false;
    bool got_error = false;
    RedisErrorType error_type = REDIS_ERROR_TYPE_INTERNAL_ERROR;
    std::string error_message;
    std::shared_ptr<PooledConnection> connection;
};

void fail(TestState* state, std::string message)
{
    state->ok = false;
    state->error = std::move(message);
}

Task<bool> waitForWaiters(RedisConnectionPool* pool,
                          size_t expected,
                          std::chrono::milliseconds timeout)
{
    for (auto waited = 0ms; waited < timeout; waited += 1ms) {
        if (pool->getStats().waiting_requests == expected) {
            co_return true;
        }
        co_await sleep(1ms);
    }
    co_return pool->getStats().waiting_requests == expected;
}

Task<bool> waitForProbe(WaiterProbe* probe, std::chrono::milliseconds timeout)
{
    for (auto waited = 0ms; waited < timeout; waited += 1ms) {
        if (probe->done.load(std::memory_order_acquire)) {
            co_return true;
        }
        co_await sleep(1ms);
    }
    co_return probe->done.load(std::memory_order_acquire);
}

Task<void> acquireInWaiter(RedisConnectionPool* pool,
                           WaiterProbe* probe,
                           std::chrono::milliseconds timeout)
{
    auto result = co_await pool->acquire().timeout(timeout);
    if (result) {
        probe->connection = result.value();
        probe->got_connection = true;
    } else {
        probe->error_type = result.error().type();
        probe->error_message = result.error().message();
        probe->got_timeout = result.error().type() == REDIS_ERROR_TYPE_TIMEOUT_ERROR;
        probe->got_error = true;
    }
    probe->done.store(true, std::memory_order_release);
}

ConnectionPoolConfig oneConnectionConfig()
{
    ConnectionPoolConfig config = ConnectionPoolConfig::create("127.0.0.1", 6379, 0, 1);
    config.initial_connections = 0;
    config.connect_timeout = 2s;
    config.acquire_timeout = 2s;
    return config;
}

bool statsMatch(const RedisConnectionPool::PoolStats& stats,
                size_t active,
                size_t available,
                size_t waiting)
{
    return stats.active_connections == active &&
           stats.available_connections == available &&
           stats.waiting_requests == waiting;
}

Task<bool> runReleaseWinsCase(IOScheduler* scheduler, TestState* state)
{
    RedisConnectionPool pool(scheduler, oneConnectionConfig());
    auto init = co_await pool.initialize().timeout(1s);
    if (!init) {
        fail(state, "release-wins init failed: " + init.error().message());
        co_return false;
    }

    auto first = co_await pool.acquire().timeout(5s);
    if (!first) {
        fail(state, "release-wins first acquire failed: " + first.error().message());
        co_return false;
    }

    WaiterProbe waiter;
    if (!scheduleTask(scheduler, acquireInWaiter(&pool, &waiter, 5s))) {
        fail(state, "release-wins failed to schedule waiter");
        co_return false;
    }
    if (!co_await waitForWaiters(&pool, 1, 1s)) {
        fail(state, "release-wins waiter did not enter queue");
        co_return false;
    }

    auto first_conn = std::move(first.value());
    pool.release(first_conn);
    first_conn.reset();

    if (!co_await waitForProbe(&waiter, 2s)) {
        fail(state, "release-wins waiter did not resume");
        co_return false;
    }
    if (!waiter.got_connection || !waiter.connection || waiter.connection->isClosed()) {
        fail(state, "release-wins waiter did not receive a live connection");
        co_return false;
    }
    if (!statsMatch(pool.getStats(), 1, 0, 0)) {
        fail(state, "release-wins stats leaked after waiter resume");
        co_return false;
    }

    pool.release(waiter.connection);
    waiter.connection.reset();
    if (!statsMatch(pool.getStats(), 0, 1, 0)) {
        fail(state, "release-wins stats leaked after final release");
        co_return false;
    }
    pool.shutdown();
    co_return true;
}

Task<bool> runTimeoutWinsCase(IOScheduler* scheduler, TestState* state)
{
    RedisConnectionPool pool(scheduler, oneConnectionConfig());
    auto init = co_await pool.initialize().timeout(1s);
    if (!init) {
        fail(state, "timeout-wins init failed: " + init.error().message());
        co_return false;
    }

    auto first = co_await pool.acquire().timeout(5s);
    if (!first) {
        fail(state, "timeout-wins first acquire failed: " + first.error().message());
        co_return false;
    }

    WaiterProbe waiter;
    if (!scheduleTask(scheduler, acquireInWaiter(&pool, &waiter, 30ms))) {
        fail(state, "timeout-wins failed to schedule waiter");
        co_return false;
    }
    if (!co_await waitForWaiters(&pool, 1, 1s)) {
        fail(state, "timeout-wins waiter did not enter queue");
        co_return false;
    }
    if (!co_await waitForProbe(&waiter, 1s)) {
        fail(state, "timeout-wins waiter did not time out");
        co_return false;
    }
    if (!waiter.got_timeout || !waiter.got_error) {
        fail(state, "timeout-wins waiter did not return timeout");
        co_return false;
    }
    if (pool.getStats().waiting_requests != 0) {
        fail(state, "timeout-wins waiter count leaked after timeout");
        co_return false;
    }

    auto first_conn = std::move(first.value());
    pool.release(first_conn);
    first_conn.reset();
    if (!statsMatch(pool.getStats(), 0, 1, 0)) {
        fail(state, "timeout-wins stats leaked after releasing held connection");
        co_return false;
    }
    pool.shutdown();
    co_return true;
}

Task<bool> runShutdownWakesWaiterCase(IOScheduler* scheduler, TestState* state)
{
    RedisConnectionPool pool(scheduler, oneConnectionConfig());
    auto init = co_await pool.initialize().timeout(1s);
    if (!init) {
        fail(state, "shutdown-waiter init failed: " + init.error().message());
        co_return false;
    }

    auto first = co_await pool.acquire().timeout(5s);
    if (!first) {
        fail(state, "shutdown-waiter first acquire failed: " + first.error().message());
        co_return false;
    }

    WaiterProbe waiter;
    if (!scheduleTask(scheduler, acquireInWaiter(&pool, &waiter, 5s))) {
        fail(state, "shutdown-waiter failed to schedule waiter");
        co_return false;
    }
    if (!co_await waitForWaiters(&pool, 1, 1s)) {
        fail(state, "shutdown-waiter waiter did not enter queue");
        co_return false;
    }

    pool.shutdown();
    if (!co_await waitForProbe(&waiter, 1s)) {
        fail(state, "shutdown-waiter waiter did not resume");
        co_return false;
    }
    if (waiter.got_connection || !waiter.got_error) {
        fail(state, "shutdown-waiter returned a connection after shutdown");
        co_return false;
    }
    if (!statsMatch(pool.getStats(), 0, 0, 0)) {
        fail(state, "shutdown-waiter stats leaked after shutdown");
        co_return false;
    }

    auto first_conn = std::move(first.value());
    first_conn.reset();
    co_return true;
}

Task<void> runPoolWaiterBlackbox(IOScheduler* scheduler, TestState* state)
{
    if (!co_await runReleaseWinsCase(scheduler, state)) {
        state->done.store(true, std::memory_order_release);
        co_return;
    }
    if (!co_await runTimeoutWinsCase(scheduler, state)) {
        state->done.store(true, std::memory_order_release);
        co_return;
    }
    if (!co_await runShutdownWakesWaiterCase(scheduler, state)) {
        state->done.store(true, std::memory_order_release);
        co_return;
    }

    state->done.store(true, std::memory_order_release);
}

} // namespace

int main()
{
    if (const int skip_code = redis_test::requireIntegrationEnabledOrSkip("redis.t24.pool.waiter.blackbox");
        skip_code != 0) {
        return skip_code;
    }

    Runtime runtime = RuntimeBuilder().ioSchedulerCount(1).computeSchedulerCount(1).build();
    runtime.start();

    auto* scheduler = runtime.getNextIOScheduler();
    if (scheduler == nullptr) {
        runtime.stop();
        std::cerr << "Failed to get IO scheduler\n";
        return 1;
    }

    TestState state;
    if (!scheduleTask(scheduler, runPoolWaiterBlackbox(scheduler, &state))) {
        runtime.stop();
        std::cerr << "failed to schedule Redis pool waiter blackbox test\n";
        return 1;
    }

    for (int i = 0; i < 1000 && !state.done.load(std::memory_order_acquire); ++i) {
        std::this_thread::sleep_for(10ms);
    }

    runtime.stop();

    if (!state.done.load(std::memory_order_acquire)) {
        std::cerr << "T24 timed out\n";
        return 1;
    }
    if (!state.ok) {
        std::cerr << state.error << "\n";
        return 1;
    }

    std::cout << "T24-RedisPoolWaiterBlackbox PASS\n";
    return 0;
}
