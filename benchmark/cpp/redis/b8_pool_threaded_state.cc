#include <atomic>
#include <chrono>
#include <expected>
#include <future>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <type_traits>

#define private public
#include <galay/cpp/galay-redis/async/conn_pool.h>
#undef private

#include <galay/cpp/galay-kernel/concurrency/async_waiter.h>
#include <galay/cpp/galay-kernel/core/runtime.h>

using namespace galay::kernel;
using namespace galay::redis;
using namespace std::chrono_literals;

namespace {

constexpr int kSchedulerCount = 4;
constexpr int kWorkerCount = 32;
constexpr int kIterationsPerWorker = 2000;
constexpr size_t kSeedConnections = 64;

template <typename T>
void addCounter(std::atomic<T>& counter, T delta = T{1}) noexcept
requires std::is_integral_v<T>
{
    const T previous = counter.fetch_add(delta, std::memory_order_acq_rel);
    if (previous > std::numeric_limits<T>::max() - delta) {
        counter.store(std::numeric_limits<T>::max(), std::memory_order_release);
    }
}

struct BenchmarkState {
    std::atomic<int> failures{0};
    std::atomic<int> completed_ops{0};
    std::atomic<int> notify_failures{0};
};

struct BenchmarkResult {
    RedisConnectionPool::PoolStats stats{};
    std::chrono::nanoseconds elapsed{0};
    int completed_ops = 0;
    int failures = 0;
    int notify_failures = 0;
};

Task<void> poolWorker(RedisConnectionPool* pool,
                      BenchmarkState* state,
                      std::shared_ptr<std::atomic<int>> remaining,
                      std::shared_ptr<AsyncWaiter<void>> done_waiter)
{
    int local_failures = 0;
    int local_completed_ops = 0;

    for (int i = 0; i < kIterationsPerWorker; ++i) {
        auto acquired = co_await pool->acquire().timeout(1s);
        if (!acquired) {
            ++local_failures;
            continue;
        }

        auto conn = acquired.value();
        if (!conn || conn->isClosed() || !conn->isHealthy()) {
            ++local_failures;
        } else {
            ++local_completed_ops;
        }
        pool->release(conn);
    }

    addCounter(state->failures, local_failures);
    addCounter(state->completed_ops, local_completed_ops);

    const int previous = remaining->fetch_sub(1, std::memory_order_acq_rel);
    if (previous == 1) {
        const bool notified = done_waiter->notify();
        if (!notified) {
            addCounter(state->notify_failures);
        }
    }
    co_return;
}

Task<void> runBenchmark(IOScheduler** schedulers,
                        std::promise<std::expected<BenchmarkResult, std::string>>* result_promise)
{
    ConnectionPoolConfig config = ConnectionPoolConfig::create("127.0.0.1", 6379, 0, kSeedConnections);
    config.initial_connections = 0;
    config.enable_health_check = false;

    RedisConnectionPool pool(schedulers[0], config);
    auto init = co_await pool.initialize().timeout(1s);
    if (!init) {
        result_promise->set_value(std::unexpected("pool init failed: " + init.error().message()));
        co_return;
    }

    for (size_t i = 0; i < kSeedConnections; ++i) {
        auto conn = pool.createConnectionSlot();
        if (!conn) {
            result_promise->set_value(std::unexpected("failed to create seeded pool connection"));
            co_return;
        }
        conn->setHealthy(true);
        const bool returned = pool.returnToAvailable(conn);
        if (!returned) {
            result_promise->set_value(std::unexpected("failed to seed pool idle queue"));
            co_return;
        }
    }

    BenchmarkState state;
    auto remaining = std::make_shared<std::atomic<int>>(kWorkerCount);
    auto done_waiter = std::make_shared<AsyncWaiter<void>>();

    const auto started = std::chrono::steady_clock::now();
    for (int worker = 0; worker < kWorkerCount; ++worker) {
        auto* scheduler = schedulers[worker % kSchedulerCount];
        if (scheduler == nullptr) {
            result_promise->set_value(std::unexpected("missing scheduler for worker"));
            co_return;
        }
        const bool scheduled = scheduleTask(
            scheduler,
            poolWorker(&pool, &state, remaining, done_waiter));
        if (!scheduled) {
            result_promise->set_value(std::unexpected("failed to schedule pool worker"));
            co_return;
        }
    }

    auto done = co_await done_waiter->wait().timeout(30s);
    const auto finished = std::chrono::steady_clock::now();
    if (!done) {
        result_promise->set_value(std::unexpected("workers timed out: " + done.error().message()));
        co_return;
    }

    BenchmarkResult result;
    result.stats = pool.getStats();
    result.elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(finished - started);
    result.completed_ops = state.completed_ops.load(std::memory_order_acquire);
    result.failures = state.failures.load(std::memory_order_acquire);
    result.notify_failures = state.notify_failures.load(std::memory_order_acquire);
    pool.shutdown();
    result_promise->set_value(result);
    co_return;
}

} // namespace

int main()
{
    Runtime runtime = RuntimeBuilder()
        .ioSchedulerCount(kSchedulerCount)
        .computeSchedulerCount(0)
        .build();
    auto started = runtime.start();
    if (!started) {
        std::cerr << "runtime start failed: " << started.error().message() << "\n";
        return 1;
    }

    IOScheduler* schedulers[kSchedulerCount] = {};
    for (int i = 0; i < kSchedulerCount; ++i) {
        schedulers[i] = runtime.getNextIOScheduler();
        if (schedulers[i] == nullptr) {
            runtime.stop();
            std::cerr << "failed to get IO scheduler\n";
            return 1;
        }
    }

    std::promise<std::expected<BenchmarkResult, std::string>> result_promise;
    auto result_future = result_promise.get_future();
    const bool scheduled = scheduleTask(schedulers[0], runBenchmark(schedulers, &result_promise));
    if (!scheduled) {
        runtime.stop();
        std::cerr << "failed to schedule benchmark\n";
        return 1;
    }

    if (result_future.wait_for(35s) != std::future_status::ready) {
        runtime.stop();
        std::cerr << "benchmark timed out\n";
        return 1;
    }

    auto result = result_future.get();
    runtime.stop();
    if (!result) {
        std::cerr << result.error() << "\n";
        return 1;
    }

    const auto elapsed_ns = result->elapsed.count();
    if (elapsed_ns <= 0 || result->failures != 0 || result->notify_failures != 0) {
        std::cerr << "invalid benchmark result: elapsed_ns=" << elapsed_ns
                  << " failures=" << result->failures
                  << " notify_failures=" << result->notify_failures << "\n";
        return 1;
    }

    const double seconds = static_cast<double>(elapsed_ns) / 1'000'000'000.0;
    const double ops_per_sec = static_cast<double>(result->completed_ops) / seconds;
    std::cout << "Redis pool threaded state benchmark\n";
    std::cout << "  schedulers: " << kSchedulerCount << "\n";
    std::cout << "  workers: " << kWorkerCount << "\n";
    std::cout << "  operations: " << result->completed_ops << "\n";
    std::cout << "  elapsed_seconds: " << seconds << "\n";
    std::cout << "  ops_per_second: " << ops_per_sec << "\n";
    std::cout << "  total_acquired: " << result->stats.total_acquired << "\n";
    std::cout << "  total_released: " << result->stats.total_released << "\n";
    std::cout << "  peak_active_connections: " << result->stats.peak_active_connections << "\n";
    return 0;
}
