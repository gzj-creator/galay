#include "redis/async/redis_client.h"
#include "redis/async/conn_pool.h"
#include "redis/async/topology_client.h"
#include <kernel/kernel/runtime.h>
#include <kernel/kernel/task.h>
#include <chrono>
#include <concepts>
#include <span>
#include <string>
#include <type_traits>
#include <utility>
#include <atomic>
#include <iostream>
#include <thread>

using namespace galay::redis;
using namespace galay::kernel;
using namespace std::chrono_literals;

template <typename T>
concept HasProtocolSendAwaitable = requires { typename T::ProtocolSendAwaitable; };

template <typename T>
concept HasProtocolRecvAwaitable = requires { typename T::ProtocolRecvAwaitable; };

template <typename T>
concept HasCore = requires { typename T::Core; };

static_assert(std::same_as<Coroutine, galay::kernel::Task<void>>);

static_assert(!HasProtocolSendAwaitable<RedisExchangeOperation>);
static_assert(!HasProtocolRecvAwaitable<RedisExchangeOperation>);
static_assert(!HasCore<RedisExchangeOperation>);
static_assert(!HasCore<RedisConnectOperation>);

static_assert(requires(RedisClient& client,
                       RedisEncodedCommand encoded,
                       std::span<const RedisCommandView> commands,
                       RedisConnectionPool& pool,
                       RedisConnectOptions options) {
    { client.command(std::move(encoded)) } -> std::same_as<RedisExchangeOperation>;
    { client.command(std::move(encoded)).timeout(std::chrono::milliseconds(1)) };
    { client.receive(1) } -> std::same_as<RedisExchangeOperation>;
    { client.receive(1).timeout(std::chrono::milliseconds(1)) };
    { client.batch(commands) } -> std::same_as<RedisExchangeOperation>;
    { client.batch(commands).timeout(std::chrono::milliseconds(1)) };
    { client.connect(std::string("127.0.0.1"), 6379, std::move(options)) } -> std::same_as<RedisConnectOperation>;
    { client.connect(std::string("127.0.0.1"), 6379, std::move(options)).timeout(std::chrono::milliseconds(1)) };
    { pool.initialize().timeout(std::chrono::milliseconds(1)) };
    { pool.acquire().timeout(std::chrono::milliseconds(1)) };
});

namespace {

struct PoolSurfaceState {
    std::atomic<bool> done{false};
    bool success = false;
    std::string failure;
};

bool waitUntil(const std::atomic<bool>& flag,
               std::chrono::milliseconds timeout = 1000ms,
               std::chrono::milliseconds step = 2ms)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (flag.load(std::memory_order_acquire)) {
            return true;
        }
        std::this_thread::sleep_for(step);
    }
    return flag.load(std::memory_order_acquire);
}

Task<void> verifyPoolSurface(PoolSurfaceState* state, IOScheduler* scheduler)
{
    ConnectionPoolConfig config = ConnectionPoolConfig::create("127.0.0.1", 6379, 2, 5);
    config.initial_connections = 2;

    RedisConnectionPool pool(scheduler, config);

    const auto init_result = co_await pool.initialize().timeout(100ms);
    if (!init_result) {
        state->failure = "initialize failed: " + init_result.error().message();
        state->done.store(true, std::memory_order_release);
        co_return;
    }

    const auto after_init = pool.getStats();
    if (after_init.total_connections != config.initial_connections) {
        state->failure = "initialize should create exactly initial_connections, got total=" +
                         std::to_string(after_init.total_connections);
        state->done.store(true, std::memory_order_release);
        co_return;
    }
    if (after_init.available_connections != config.initial_connections) {
        state->failure = "initialize should make all initial connections immediately available, got available=" +
                         std::to_string(after_init.available_connections);
        state->done.store(true, std::memory_order_release);
        co_return;
    }

    const auto acquire_result = co_await pool.acquire().timeout(100ms);
    if (!acquire_result) {
        state->failure = "acquire failed: " + acquire_result.error().message();
        state->done.store(true, std::memory_order_release);
        co_return;
    }

    auto conn = acquire_result.value();
    const auto after_acquire = pool.getStats();
    if (after_acquire.total_connections != config.initial_connections) {
        state->failure = "first acquire should reuse initialized connections instead of expanding the pool, got total=" +
                         std::to_string(after_acquire.total_connections);
        state->done.store(true, std::memory_order_release);
        co_return;
    }
    if (after_acquire.available_connections + 1 != config.initial_connections) {
        state->failure = "acquire should consume exactly one available connection, got available=" +
                         std::to_string(after_acquire.available_connections);
        state->done.store(true, std::memory_order_release);
        co_return;
    }

    pool.release(std::move(conn));

    const auto after_release = pool.getStats();
    if (after_release.available_connections != config.initial_connections) {
        state->failure = "release should restore the available connection count, got available=" +
                         std::to_string(after_release.available_connections);
        state->done.store(true, std::memory_order_release);
        co_return;
    }

    pool.shutdown();
    state->success = true;
    state->done.store(true, std::memory_order_release);
}

} // namespace

int main()
{
    Runtime runtime = RuntimeBuilder()
        .ioSchedulerCount(1)
        .computeSchedulerCount(1)
        .build();
    runtime.start();

    PoolSurfaceState state;
    scheduleTask(runtime.getNextIOScheduler(), verifyPoolSurface(&state, runtime.getNextIOScheduler()));

    const bool completed = waitUntil(state.done, 2000ms);

    runtime.stop();

    if (!completed) {
        std::cerr << "T15 timed out\n";
        return 1;
    }
    if (!state.success) {
        std::cerr << "T15 failure: " << state.failure << "\n";
        return 1;
    }

    std::cout << "T15-AwaitableSurface PASS\n";
    return 0;
}
