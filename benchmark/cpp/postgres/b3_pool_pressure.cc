#include "common/config.h"

#include <galay/cpp/galay-kernel/async/async_waiter.h>
#include <galay/cpp/galay-kernel/core/runtime.h>
#include <galay/cpp/galay-postgres/async/conn_pool.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

using namespace galay::kernel;
using namespace galay::postgres;
using namespace std::chrono_literals;

namespace
{

struct State
{
    std::mutex samples_mutex;
    std::vector<uint64_t> samples_ns;
    std::atomic<uint64_t> succeeded{0};
    std::atomic<uint64_t> failed{0};
    std::atomic<uint64_t> acquire_failures{0};
    std::atomic<uint64_t> query_failures{0};
    std::atomic<uint64_t> schedule_failures{0};
    std::chrono::steady_clock::time_point measurement_started{};
    std::chrono::steady_clock::time_point measurement_finished{};
};

double percentileMs(const std::vector<uint64_t>& sorted_samples, double fraction)
{
    if (sorted_samples.empty()) {
        return 0.0;
    }
    const size_t index = static_cast<size_t>(
        fraction * static_cast<double>(sorted_samples.size() - 1));
    return static_cast<double>(sorted_samples[index]) / 1e6;
}

void markWorkerComplete(State* state,
                        const std::shared_ptr<std::atomic<size_t>>& remaining,
                        const std::shared_ptr<AsyncWaiter<void>>& done_waiter)
{
    if (remaining->fetch_sub(1, std::memory_order_acq_rel) == 1) {
        state->measurement_finished = std::chrono::steady_clock::now();
        (void)done_waiter->notify();
    }
}

Task<bool> warmPool(PostgresConnectionPool* pool,
                    const postgres_benchmark::Config& config)
{
    std::vector<PostgresPoolLease> leases;
    leases.reserve(config.pool_size);
    for (size_t index = 0; index < config.pool_size; ++index) {
        auto acquired = co_await pool->lease();
        if (!acquired || !acquired->has_value()) {
            co_return false;
        }
        leases.emplace_back(std::move(acquired->value()));
    }

    for (auto& lease : leases) {
        auto result = co_await lease->query(config.sql).timeout(5s);
        if (!result || !result->has_value()) {
            co_return false;
        }
    }
    co_return true;
}

Task<void> worker(PostgresConnectionPool* pool,
                  State* state,
                  const std::shared_ptr<std::atomic<size_t>>& remaining,
                  const std::shared_ptr<AsyncWaiter<void>>& done_waiter,
                  postgres_benchmark::Config config)
{
    std::vector<uint64_t> local_samples;
    local_samples.reserve(config.queries);
    uint64_t local_succeeded = 0;
    uint64_t local_failed = 0;
    uint64_t local_acquire_failures = 0;
    uint64_t local_query_failures = 0;

    for (size_t index = 0; index < config.queries; ++index) {
        const auto started = std::chrono::steady_clock::now();
        auto acquired = co_await pool->lease();
        if (!acquired || !acquired->has_value()) {
            const auto finished = std::chrono::steady_clock::now();
            local_samples.push_back(static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(finished - started).count()));
            ++local_failed;
            ++local_acquire_failures;
            continue;
        }

        PostgresPoolLease lease = std::move(acquired->value());
        auto result = co_await lease->query(config.sql).timeout(5s);
        const auto finished = std::chrono::steady_clock::now();
        local_samples.push_back(static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(finished - started).count()));
        if (!result || !result->has_value()) {
            ++local_failed;
            ++local_query_failures;
            continue;
        }
        ++local_succeeded;
    }

    state->succeeded.fetch_add(local_succeeded, std::memory_order_relaxed);
    state->failed.fetch_add(local_failed, std::memory_order_relaxed);
    state->acquire_failures.fetch_add(local_acquire_failures, std::memory_order_relaxed);
    state->query_failures.fetch_add(local_query_failures, std::memory_order_relaxed);
    {
        std::lock_guard lock(state->samples_mutex);
        state->samples_ns.insert(state->samples_ns.end(),
                                 local_samples.begin(),
                                 local_samples.end());
    }
    markWorkerComplete(state, remaining, done_waiter);
}

Task<bool> run(IOScheduler* scheduler,
               State* state,
               postgres_benchmark::Config config)
{
    PostgresConnectionPoolConfig pool_config;
    pool_config.postgres_config = PostgresConfig::create(config.host, config.port,
                                                         config.user, config.password,
                                                         config.database);
    pool_config.async_config = AsyncPostgresConfig::withTimeout(5s, 5s);
    pool_config.min_connections = 0;
    pool_config.max_connections = config.pool_size;
    auto pool = std::make_shared<PostgresConnectionPool>(scheduler, std::move(pool_config));

    if (!(co_await warmPool(pool.get(), config))) {
        co_return false;
    }

    auto remaining = std::make_shared<std::atomic<size_t>>(config.clients);
    auto done_waiter = std::make_shared<AsyncWaiter<void>>();
    for (size_t index = 0; index < config.clients; ++index) {
        if (!scheduleTask(scheduler, worker(pool.get(), state, remaining, done_waiter, config))) {
            state->failed.fetch_add(config.queries, std::memory_order_relaxed);
            state->schedule_failures.fetch_add(1, std::memory_order_relaxed);
            (void)remaining->fetch_sub(1, std::memory_order_acq_rel);
        }
    }

    state->measurement_started = std::chrono::steady_clock::now();
    if (remaining->load(std::memory_order_acquire) == 0) {
        state->measurement_finished = state->measurement_started;
        (void)done_waiter->notify();
    }
    const auto completed = co_await done_waiter->wait();
    co_return completed.has_value();
}

} // namespace

int main(int argc, char** argv)
{
    auto config = postgres_benchmark::loadConfig();
    if (!postgres_benchmark::parseArgs(config, argc, argv)) {
        postgres_benchmark::printUsage(argv[0]);
        return 2;
    }
    postgres_benchmark::printConfig(config);

    Runtime runtime = RuntimeBuilder().ioSchedulerCount(1).parallelSchedulerCount(0).build();
    const auto started = runtime.start();
    if (!started) {
        std::cerr << "runtime start failed: " << started.error().message() << '\n';
        return 1;
    }
    auto* scheduler = runtime.getNextIOScheduler();
    if (scheduler == nullptr) {
        runtime.stop();
        std::cerr << "runtime has no IO scheduler\n";
        return 1;
    }

    State state;
    state.samples_ns.reserve(config.clients * config.queries);
    auto completed = runtime.blockOnIO(run(scheduler, &state, config));
    runtime.stop();
    if (!completed || !*completed) {
        std::cerr << (completed ? "pool warmup or completion failed"
                                : completed.error().message()) << '\n';
        return 1;
    }

    std::sort(state.samples_ns.begin(), state.samples_ns.end());
    const uint64_t succeeded = state.succeeded.load(std::memory_order_relaxed);
    const uint64_t failed = state.failed.load(std::memory_order_relaxed);
    const double seconds = std::chrono::duration<double>(
        state.measurement_finished - state.measurement_started).count();
    std::cout << "\n=== Galay PostgreSQL Async Pool Pressure Summary ===\n"
              << "clients: " << config.clients << '\n'
              << "pool_connections: " << config.pool_size << '\n'
              << "warmup_queries_per_connection: 1\n"
              << "queries_per_client: " << config.queries << '\n'
              << "total_queries: " << succeeded + failed << '\n'
              << "success: " << succeeded << '\n'
              << "failed: " << failed << '\n'
              << "acquire_failures: "
              << state.acquire_failures.load(std::memory_order_relaxed) << '\n'
              << "query_failures: "
              << state.query_failures.load(std::memory_order_relaxed) << '\n'
              << "schedule_failures: "
              << state.schedule_failures.load(std::memory_order_relaxed) << '\n'
              << "elapsed_sec: " << seconds << '\n'
              << "qps: " << (seconds > 0.0 ? static_cast<double>(succeeded) / seconds : 0.0)
              << '\n'
              << "p50_acquire_query_latency_ms: " << percentileMs(state.samples_ns, 0.50) << '\n'
              << "p95_acquire_query_latency_ms: " << percentileMs(state.samples_ns, 0.95) << '\n'
              << "p99_acquire_query_latency_ms: " << percentileMs(state.samples_ns, 0.99) << '\n'
              << "max_acquire_query_latency_ms: " << percentileMs(state.samples_ns, 1.0) << '\n';
    return failed == 0 ? 0 : 1;
}
