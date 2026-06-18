#include "galay-redis/async/conn_pool.h"

#include <galay-kernel/concurrency/async_waiter.h>
#include <galay-kernel/core/runtime.h>

#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_set>

using namespace galay::kernel;
using namespace galay::redis;
using namespace std::chrono_literals;

namespace {

constexpr int kWorkerCount = 20;
constexpr int kIterationsPerWorker = 200;
constexpr size_t kMinConnections = 4;
constexpr size_t kMaxConnections = 40;

struct SharedState {
    std::atomic<int> acquire_failures{0};
    std::atomic<int> acquire_internal_errors{0};
    std::atomic<int> acquire_timeouts{0};
    std::atomic<int> command_failures{0};
    std::mutex connected_mutex;
    std::unordered_set<RedisClient*> connected_clients;
};

Task<void> workerTask(RedisConnectionPool* pool,
                     SharedState* state,
                     int worker_id,
                     std::shared_ptr<std::atomic<int>> remaining,
                     std::shared_ptr<AsyncWaiter<void>> done_waiter)
{
    RedisCommandBuilder command_builder;
    int local_acquire_failures = 0;
    int local_acquire_internal_errors = 0;
    int local_acquire_timeouts = 0;
    int local_command_failures = 0;

    for (int i = 0; i < kIterationsPerWorker; ++i) {
        auto conn_result = co_await pool->acquire().timeout(5s);
        if (!conn_result) {
            ++local_acquire_failures;
            if (conn_result.error().type() == REDIS_ERROR_TYPE_INTERNAL_ERROR) {
                ++local_acquire_internal_errors;
            } else if (conn_result.error().type() == REDIS_ERROR_TYPE_TIMEOUT_ERROR) {
                ++local_acquire_timeouts;
            }
            continue;
        }

        auto conn = conn_result.value();
        auto* client = conn->get();

        bool need_connect = false;
        {
            std::lock_guard<std::mutex> lock(state->connected_mutex);
            need_connect = state->connected_clients.find(client) == state->connected_clients.end();
        }

        if (need_connect || client->isClosed()) {
            auto connect_result = co_await client->connect("127.0.0.1", 6379).timeout(5s);
            if (!connect_result) {
                ++local_command_failures;
                pool->release(conn);
                continue;
            }

            std::lock_guard<std::mutex> lock(state->connected_mutex);
            state->connected_clients.insert(client);
        }

        const std::string key =
            "test:pool:acquire:" + std::to_string(worker_id) + ":" + std::to_string(i);
        const std::string value = "value_" + std::to_string(i);

        auto set_result = co_await client->command(command_builder.set(key, value)).timeout(5s);
        auto get_result = co_await client->command(command_builder.get(key)).timeout(5s);
        if (!set_result || !set_result.value() || !get_result || !get_result.value()) {
            ++local_command_failures;
        }

        pool->release(conn);
    }

    state->acquire_failures.fetch_add(local_acquire_failures, std::memory_order_relaxed);
    state->acquire_internal_errors.fetch_add(local_acquire_internal_errors, std::memory_order_relaxed);
    state->acquire_timeouts.fetch_add(local_acquire_timeouts, std::memory_order_relaxed);
    state->command_failures.fetch_add(local_command_failures, std::memory_order_relaxed);

    if (remaining->fetch_sub(1, std::memory_order_acq_rel) == 1) {
        done_waiter->notify();
    }
}

Task<void> runTest(IOScheduler* scheduler, std::promise<int>* exit_code)
{
    auto config = ConnectionPoolConfig::create("127.0.0.1", 6379, kMinConnections, kMaxConnections);
    config.initial_connections = kMinConnections;

    RedisConnectionPool pool(scheduler, config);
    auto init_result = co_await pool.initialize().timeout(5s);
    if (!init_result) {
        std::cerr << "Failed to initialize pool: " << init_result.error().message() << std::endl;
        exit_code->set_value(1);
        co_return;
    }

    SharedState state;
    auto remaining = std::make_shared<std::atomic<int>>(kWorkerCount);
    auto done_waiter = std::make_shared<AsyncWaiter<void>>();

    for (int worker_id = 0; worker_id < kWorkerCount; ++worker_id) {
        if (!scheduleTask(scheduler, workerTask(&pool, &state, worker_id, remaining, done_waiter))) {
            std::cerr << "Failed to schedule worker " << worker_id << std::endl;
            exit_code->set_value(1);
            co_return;
        }
    }

    auto done = co_await done_waiter->wait().timeout(45s);
    if (!done) {
        std::cerr << "Workers timed out: " << done.error().message() << std::endl;
        exit_code->set_value(1);
        co_return;
    }

    const auto stats = pool.getStats();
    const int acquire_failures = state.acquire_failures.load(std::memory_order_relaxed);
    const int acquire_internal_errors = state.acquire_internal_errors.load(std::memory_order_relaxed);
    const int acquire_timeouts = state.acquire_timeouts.load(std::memory_order_relaxed);
    const int command_failures = state.command_failures.load(std::memory_order_relaxed);

    std::cout << "acquire_failures=" << acquire_failures << std::endl;
    std::cout << "acquire_internal_errors=" << acquire_internal_errors << std::endl;
    std::cout << "acquire_timeouts=" << acquire_timeouts << std::endl;
    std::cout << "command_failures=" << command_failures << std::endl;
    std::cout << "total_acquired=" << stats.total_acquired << std::endl;
    std::cout << "total_released=" << stats.total_released << std::endl;

    pool.shutdown();

    if (acquire_failures != 0 || command_failures != 0 ||
        stats.total_acquired != static_cast<uint64_t>(kWorkerCount * kIterationsPerWorker)) {
        exit_code->set_value(1);
        co_return;
    }

    exit_code->set_value(0);
}

}  // namespace

int main()
{
    try {
        Runtime runtime;
        runtime.start();

        auto* scheduler = runtime.getNextIOScheduler();
        if (scheduler == nullptr) {
            std::cerr << "Failed to get IO scheduler" << std::endl;
            return 1;
        }

        std::promise<int> exit_code_promise;
        auto exit_code_future = exit_code_promise.get_future();
        if (!scheduleTask(scheduler, runTest(scheduler, &exit_code_promise))) {
            std::cerr << "Failed to schedule test coroutine" << std::endl;
            runtime.stop();
            return 1;
        }

        if (exit_code_future.wait_for(60s) != std::future_status::ready) {
            std::cerr << "Test timed out" << std::endl;
            runtime.stop();
            return 1;
        }

        const int exit_code = exit_code_future.get();
        runtime.stop();
        return exit_code;
    } catch (const std::exception& ex) {
        std::cerr << "Unhandled exception: " << ex.what() << std::endl;
        return 1;
    }
}
