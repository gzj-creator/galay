#include <atomic>
#include <chrono>
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
constexpr int kWorkerCount = 16;
constexpr int kIterationsPerWorker = 250;
constexpr size_t kSeedConnections = 32;

template <typename T>
void addCounter(std::atomic<T>& counter, T delta = T{1}) noexcept
requires std::is_integral_v<T>
{
    const T previous = counter.fetch_add(delta, std::memory_order_acq_rel);
    if (previous > std::numeric_limits<T>::max() - delta) {
        counter.store(std::numeric_limits<T>::max(), std::memory_order_release);
    }
}

struct SharedState {
    std::atomic<int> acquire_failures{0};
    std::atomic<int> invalid_connections{0};
    std::atomic<int> completed_ops{0};
    std::atomic<int> notify_failures{0};
};

Task<void> poolWorker(RedisConnectionPool* pool,
                      SharedState* state,
                      std::shared_ptr<std::atomic<int>> remaining,
                      std::shared_ptr<AsyncWaiter<void>> done_waiter)
{
    int local_acquire_failures = 0;
    int local_invalid_connections = 0;
    int local_completed_ops = 0;

    for (int i = 0; i < kIterationsPerWorker; ++i) {
        auto result = co_await pool->acquire().timeout(1s);
        if (!result) {
            ++local_acquire_failures;
            continue;
        }

        auto conn = result.value();
        if (!conn || conn->isClosed() || !conn->isHealthy()) {
            ++local_invalid_connections;
        } else {
            ++local_completed_ops;
        }
        pool->release(conn);
    }

    addCounter(state->acquire_failures, local_acquire_failures);
    addCounter(state->invalid_connections, local_invalid_connections);
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

Task<void> runPoolThreadedState(IOScheduler** schedulers, std::promise<int>* exit_code)
{
    ConnectionPoolConfig config = ConnectionPoolConfig::create("127.0.0.1", 6379, 0, kSeedConnections);
    config.initial_connections = 0;
    config.enable_health_check = false;

    RedisConnectionPool pool(schedulers[0], config);
    auto init = co_await pool.initialize().timeout(1s);
    if (!init) {
        std::cerr << "pool init failed: " << init.error().message() << "\n";
        exit_code->set_value(1);
        co_return;
    }

    for (size_t i = 0; i < kSeedConnections; ++i) {
        auto conn = pool.createConnectionSlot();
        if (!conn) {
            std::cerr << "failed to create seeded pool connection\n";
            exit_code->set_value(1);
            co_return;
        }
        conn->setHealthy(true);
        const bool returned = pool.returnToAvailable(conn);
        if (!returned) {
            std::cerr << "failed to seed pool idle queue\n";
            exit_code->set_value(1);
            co_return;
        }
    }

    SharedState state;
    auto remaining = std::make_shared<std::atomic<int>>(kWorkerCount);
    auto done_waiter = std::make_shared<AsyncWaiter<void>>();

    for (int worker = 0; worker < kWorkerCount; ++worker) {
        auto* scheduler = schedulers[worker % kSchedulerCount];
        if (scheduler == nullptr) {
            std::cerr << "missing scheduler for worker\n";
            exit_code->set_value(1);
            co_return;
        }
        const bool scheduled = scheduleTask(
            scheduler,
            poolWorker(&pool, &state, remaining, done_waiter));
        if (!scheduled) {
            std::cerr << "failed to schedule pool worker\n";
            exit_code->set_value(1);
            co_return;
        }
    }

    auto done = co_await done_waiter->wait().timeout(10s);
    if (!done) {
        std::cerr << "workers timed out: " << done.error().message() << "\n";
        exit_code->set_value(1);
        co_return;
    }

    const auto stats = pool.getStats();
    const int expected_ops = kWorkerCount * kIterationsPerWorker;
    const int failures = state.acquire_failures.load(std::memory_order_acquire);
    const int invalid = state.invalid_connections.load(std::memory_order_acquire);
    const int completed = state.completed_ops.load(std::memory_order_acquire);
    const int notify_failures = state.notify_failures.load(std::memory_order_acquire);

    pool.shutdown();

    if (failures != 0 || invalid != 0 || notify_failures != 0 || completed != expected_ops) {
        std::cerr << "pool threaded state mismatch: completed=" << completed
                  << " expected=" << expected_ops
                  << " acquire_failures=" << failures
                  << " invalid=" << invalid
                  << " notify_failures=" << notify_failures << "\n";
        exit_code->set_value(1);
        co_return;
    }
    if (stats.active_connections != 0 ||
        stats.waiting_requests != 0 ||
        stats.total_acquired != static_cast<uint64_t>(expected_ops) ||
        stats.total_released != static_cast<uint64_t>(expected_ops)) {
        std::cerr << "pool stats mismatch after threaded acquire/release: active="
                  << stats.active_connections
                  << " waiting=" << stats.waiting_requests
                  << " acquired=" << stats.total_acquired
                  << " released=" << stats.total_released << "\n";
        exit_code->set_value(1);
        co_return;
    }

    std::cout << "T28-RedisPoolThreadedState PASS ops=" << expected_ops << "\n";
    exit_code->set_value(0);
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

    std::promise<int> exit_code_promise;
    auto exit_code_future = exit_code_promise.get_future();
    const bool scheduled = scheduleTask(
        schedulers[0],
        runPoolThreadedState(schedulers, &exit_code_promise));
    if (!scheduled) {
        runtime.stop();
        std::cerr << "failed to schedule threaded state test\n";
        return 1;
    }

    if (exit_code_future.wait_for(15s) != std::future_status::ready) {
        runtime.stop();
        std::cerr << "threaded state test timed out\n";
        return 1;
    }

    const int exit_code = exit_code_future.get();
    runtime.stop();
    return exit_code;
}
