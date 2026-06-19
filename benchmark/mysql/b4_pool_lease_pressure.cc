#include <galay-kernel/concurrency/async_waiter.h>
#include <galay-kernel/core/runtime.h>

#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>

#include "common/config.h"
#include "galay-mysql/async/conn_pool.h"

using namespace galay::kernel;
using namespace galay::mysql;
using namespace std::chrono_literals;

namespace
{

struct LeaseBenchmarkState {
    std::atomic<size_t> finished_workers{0};
    std::atomic<uint64_t> success{0};
    std::atomic<uint64_t> failed{0};
    std::atomic<uint64_t> acquire_failures{0};
    std::atomic<uint64_t> query_failures{0};
    std::atomic<uint64_t> latency_ns{0};
    std::atomic<uint64_t> schedule_failures{0};
    std::atomic<bool> timed_out{false};
};

Task<void> runLeaseWorker(MysqlConnectionPool* pool,
                          LeaseBenchmarkState* state,
                          std::shared_ptr<std::atomic<size_t>> remaining,
                          std::shared_ptr<AsyncWaiter<void>> done_waiter,
                          mysql_benchmark::DbBenchmarkConfig cfg)
{
    for (size_t i = 0; i < cfg.queries_per_client; ++i) {
        const auto started = std::chrono::steady_clock::now();
        auto acquired = co_await pool->acquireLease();
        if (!acquired || !acquired->has_value()) {
            state->failed.fetch_add(1, std::memory_order_relaxed);
            state->acquire_failures.fetch_add(1, std::memory_order_relaxed);
            continue;
        }

        auto query = co_await acquired->value()->query(cfg.sql);
        const auto finished = std::chrono::steady_clock::now();
        state->latency_ns.fetch_add(
            static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                finished - started).count()),
            std::memory_order_relaxed);

        if (!query || !query->has_value()) {
            state->failed.fetch_add(1, std::memory_order_relaxed);
            state->query_failures.fetch_add(1, std::memory_order_relaxed);
            continue;
        }

        state->success.fetch_add(1, std::memory_order_relaxed);
    }

    state->finished_workers.fetch_add(1, std::memory_order_release);
    if (remaining->fetch_sub(1, std::memory_order_acq_rel) == 1) {
        done_waiter->notify();
    }
}

Task<void> runLeaseBenchmark(IOScheduler* scheduler,
                             LeaseBenchmarkState* state,
                             mysql_benchmark::DbBenchmarkConfig cfg)
{
    MysqlConnectionPoolConfig pool_cfg;
    pool_cfg.mysql_config = MysqlConfig::create(cfg.host,
                                                cfg.port,
                                                cfg.user,
                                                cfg.password,
                                                cfg.database);
    pool_cfg.async_config = AsyncMysqlConfig::withTimeout(3s, 5s);
    pool_cfg.min_connections = 0;
    pool_cfg.max_connections = cfg.batch_size == 0 ? cfg.clients : cfg.batch_size;

    MysqlConnectionPool pool(scheduler, pool_cfg);
    auto remaining = std::make_shared<std::atomic<size_t>>(cfg.clients);
    auto done_waiter = std::make_shared<AsyncWaiter<void>>();

    for (size_t worker = 0; worker < cfg.clients; ++worker) {
        if (!scheduleTask(scheduler,
                          runLeaseWorker(&pool, state, remaining, done_waiter, cfg))) {
            state->failed.fetch_add(cfg.queries_per_client, std::memory_order_relaxed);
            state->schedule_failures.fetch_add(1, std::memory_order_relaxed);
            state->finished_workers.fetch_add(1, std::memory_order_release);
            if (remaining->fetch_sub(1, std::memory_order_acq_rel) == 1) {
                done_waiter->notify();
            }
        }
    }

    auto done = co_await done_waiter->wait().timeout(std::chrono::seconds(cfg.timeout_seconds));
    if (!done) {
        state->timed_out.store(true, std::memory_order_release);
    }
}

} // namespace

int main(int argc, char* argv[])
{
    auto cfg = mysql_benchmark::loadDbBenchmarkConfig();
    if (!mysql_benchmark::parseArgs(cfg, argc, argv, std::cerr)) {
        mysql_benchmark::printUsage(argv[0]);
        return 2;
    }

    mysql_benchmark::printConfig(cfg);
    std::cout << "Running async pool lease pressure benchmark..." << std::endl;

    Runtime runtime = RuntimeBuilder()
        .ioSchedulerCount(1)
        .computeSchedulerCount(0)
        .build();
    runtime.start();

    auto* scheduler = runtime.getNextIOScheduler();
    if (scheduler == nullptr) {
        runtime.stop();
        std::cerr << "failed to get IO scheduler" << std::endl;
        return 1;
    }

    LeaseBenchmarkState state;
    const auto started = std::chrono::steady_clock::now();
    auto result = runtime.blockOn(runLeaseBenchmark(scheduler, &state, cfg));
    const auto finished = std::chrono::steady_clock::now();
    runtime.stop();

    if (!result) {
        std::cerr << "runtime failed: " << result.error().message() << std::endl;
        return 1;
    }

    const uint64_t success = state.success.load(std::memory_order_relaxed);
    const uint64_t failed = state.failed.load(std::memory_order_relaxed);
    const uint64_t total = success + failed;
    const double elapsed_sec =
        static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            finished - started).count()) / 1e9;
    const double qps = elapsed_sec > 0.0 ? static_cast<double>(success) / elapsed_sec : 0.0;
    const double avg_latency_ms = total > 0
        ? (static_cast<double>(state.latency_ns.load(std::memory_order_relaxed)) /
           static_cast<double>(total)) /
              1e6
        : 0.0;

    std::cout << "\n=== B4 Async Pool Lease Pressure Summary ===\n"
              << "clients: " << cfg.clients << '\n'
              << "pool_max_connections: " << (cfg.batch_size == 0 ? cfg.clients : cfg.batch_size) << '\n'
              << "queries_per_client: " << cfg.queries_per_client << '\n'
              << "success: " << success << '\n'
              << "failed: " << failed << '\n'
              << "acquire_failures: " << state.acquire_failures.load(std::memory_order_relaxed) << '\n'
              << "query_failures: " << state.query_failures.load(std::memory_order_relaxed) << '\n'
              << "schedule_failures: " << state.schedule_failures.load(std::memory_order_relaxed) << '\n'
              << "timed_out: " << state.timed_out.load(std::memory_order_acquire) << '\n'
              << "elapsed_sec: " << elapsed_sec << '\n'
              << "qps: " << qps << '\n'
              << "avg_acquire_query_latency_ms: " << avg_latency_ms << std::endl;

    return failed == 0 && !state.timed_out.load(std::memory_order_acquire) ? 0 : 1;
}
