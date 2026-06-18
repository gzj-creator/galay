#include "common/config.h"
#include "galay-redis/async/conn_pool.h"
#include <galay-kernel/concurrency/async_waiter.h>
#include <galay-kernel/core/runtime.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <string>
#include <unordered_set>

using namespace galay::kernel;
using namespace galay::redis;

namespace {

struct BenchmarkOptions {
    std::string host = galay::redis::example::kDefaultRedisHost;
    int port = galay::redis::example::kDefaultRedisPort;
    int workers = 20;
    int operations = 300;
    int min_connections = 4;
    int max_connections = 20;
    bool verbose = true;
};

struct BenchmarkResult {
    bool finished = false;
    bool init_success = false;
    std::string init_error;
    std::int64_t duration_ms = 0;
    std::int64_t success = 0;
    std::int64_t error = 0;
    std::int64_t timeout = 0;
    RedisConnectionPool::PoolStats pool_stats{};
};

struct CompletionState {
    std::mutex mutex;
    std::condition_variable cv;
    bool done = false;
    int exit_code = 2;
};

struct SharedStats {
    std::atomic<std::int64_t> success{0};
    std::atomic<std::int64_t> error{0};
    std::atomic<std::int64_t> timeout{0};
    std::mutex connected_mutex;
    std::unordered_set<RedisClient*> connected_clients;
};

bool parseInt(const std::string& text, int& value)
{
    try {
        size_t used = 0;
        const int parsed = std::stoi(text, &used);
        if (used != text.size()) return false;
        value = parsed;
        return true;
    } catch (...) {
        return false;
    }
}

void printUsage(const char* program)
{
    std::cout << "Usage: " << program
              << " [-h host] [-p port] [-c workers] [-n operations] "
                 "[-m min_connections] [-x max_connections] [-q]"
              << std::endl;
}

bool parseArgs(int argc, char* argv[], BenchmarkOptions& options, bool& show_help)
{
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help") {
            show_help = true;
            return false;
        }
        if (arg == "-q" || arg == "--quiet") {
            options.verbose = false;
            continue;
        }
        if (i + 1 >= argc) {
            std::cerr << "Missing value for argument: " << arg << std::endl;
            return false;
        }

        const std::string value = argv[++i];
        if (arg == "-h" || arg == "--host") {
            options.host = value;
            continue;
        }
        if (arg == "-p" || arg == "--port") {
            if (!parseInt(value, options.port) || options.port <= 0 || options.port > 65535) {
                std::cerr << "Invalid port: " << value << std::endl;
                return false;
            }
            continue;
        }
        if (arg == "-c" || arg == "--workers") {
            if (!parseInt(value, options.workers) || options.workers <= 0) {
                std::cerr << "Invalid workers: " << value << std::endl;
                return false;
            }
            continue;
        }
        if (arg == "-n" || arg == "--operations") {
            if (!parseInt(value, options.operations) || options.operations <= 0) {
                std::cerr << "Invalid operations: " << value << std::endl;
                return false;
            }
            continue;
        }
        if (arg == "-m" || arg == "--min-connections") {
            if (!parseInt(value, options.min_connections) || options.min_connections <= 0) {
                std::cerr << "Invalid min-connections: " << value << std::endl;
                return false;
            }
            continue;
        }
        if (arg == "-x" || arg == "--max-connections") {
            if (!parseInt(value, options.max_connections) || options.max_connections <= 0) {
                std::cerr << "Invalid max-connections: " << value << std::endl;
                return false;
            }
            continue;
        }

        std::cerr << "Unknown argument: " << arg << std::endl;
        return false;
    }

    if (options.min_connections > options.max_connections) {
        std::cerr << "min-connections must be <= max-connections" << std::endl;
        return false;
    }
    return true;
}

bool isTimeoutError(const RedisError& error)
{
    return error.type() == REDIS_ERROR_TYPE_TIMEOUT_ERROR;
}

void countCommandResult(
    const std::expected<std::optional<std::vector<RedisValue>>, RedisError>& result,
    std::int64_t& success,
    std::int64_t& error,
    std::int64_t& timeout)
{
    if (result && result.value()) {
        ++success;
        return;
    }

    if (!result) {
        if (isTimeoutError(result.error())) {
            ++timeout;
        } else {
            ++error;
        }
        return;
    }

    ++error;
}

Task<void> poolWorker(
    std::shared_ptr<RedisConnectionPool> pool,
    const BenchmarkOptions* options,
    SharedStats* stats,
    int worker_id,
    std::shared_ptr<std::atomic<int>> remaining,
    std::shared_ptr<AsyncWaiter<void>> done_waiter)
{
    RedisCommandBuilder command_builder;
    std::int64_t local_success = 0;
    std::int64_t local_error = 0;
    std::int64_t local_timeout = 0;

    for (int i = 0; i < options->operations; ++i) {
        auto acquire_result = co_await pool->acquire().timeout(std::chrono::seconds(5));
        if (!acquire_result) {
            if (isTimeoutError(acquire_result.error())) {
                ++local_timeout;
            } else {
                ++local_error;
            }
            continue;
        }

        auto conn = acquire_result.value();
        auto* client = conn->get();

        bool need_connect = false;
        {
            std::lock_guard<std::mutex> lock(stats->connected_mutex);
            need_connect = (stats->connected_clients.find(client) == stats->connected_clients.end());
        }

        if (need_connect) {
            auto connect_result = co_await client->connect(options->host, options->port).timeout(std::chrono::seconds(5));
            if (!connect_result) {
                if (isTimeoutError(connect_result.error())) {
                    ++local_timeout;
                } else {
                    ++local_error;
                }
                pool->release(conn);
                continue;
            }

            std::lock_guard<std::mutex> lock(stats->connected_mutex);
            stats->connected_clients.insert(client);
        }

        const std::string key = "bench:pool:" + std::to_string(worker_id) + ":" + std::to_string(i);
        const std::string value = "value_" + std::to_string(i);

        auto set_result = co_await client->command(command_builder.set(key, value))
                              .timeout(std::chrono::seconds(5));
        countCommandResult(set_result, local_success, local_error, local_timeout);

        auto get_result = co_await client->command(command_builder.get(key))
                              .timeout(std::chrono::seconds(5));
        countCommandResult(get_result, local_success, local_error, local_timeout);

        pool->release(conn);
    }

    stats->success.fetch_add(local_success, std::memory_order_relaxed);
    stats->error.fetch_add(local_error, std::memory_order_relaxed);
    stats->timeout.fetch_add(local_timeout, std::memory_order_relaxed);

    if (options->verbose) {
        std::cout << "Worker " << worker_id << " finished" << std::endl;
    }

    if (remaining->fetch_sub(1, std::memory_order_relaxed) == 1) {
        done_waiter->notify();
    }
}

Task<void> runBenchmark(
    IOScheduler* scheduler,
    const BenchmarkOptions* options,
    BenchmarkResult* result,
    CompletionState* completion)
{
    auto pool_config = ConnectionPoolConfig::create(
        options->host,
        options->port,
        static_cast<size_t>(options->min_connections),
        static_cast<size_t>(options->max_connections));
    pool_config.initial_connections = static_cast<size_t>(options->min_connections);

    auto pool = std::make_shared<RedisConnectionPool>(scheduler, pool_config);

    auto init_result = co_await pool->initialize().timeout(std::chrono::seconds(10));
    if (!init_result) {
        result->init_success = false;
        result->init_error = init_result.error().message();

        std::lock_guard<std::mutex> lock(completion->mutex);
        completion->done = true;
        completion->exit_code = 1;
        completion->cv.notify_one();
        co_return;
    }
    result->init_success = true;

    SharedStats stats;
    auto remaining = std::make_shared<std::atomic<int>>(options->workers);
    auto done_waiter = std::make_shared<AsyncWaiter<void>>();

    const auto start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < options->workers; ++i) {
        scheduleTask(scheduler, poolWorker(pool, options, &stats, i, remaining, done_waiter));
    }

    auto all_done = co_await done_waiter->wait().timeout(std::chrono::seconds(180));
    const auto end = std::chrono::high_resolution_clock::now();

    result->finished = all_done.has_value();
    result->duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    result->success = stats.success.load(std::memory_order_relaxed);
    result->error = stats.error.load(std::memory_order_relaxed);
    result->timeout = stats.timeout.load(std::memory_order_relaxed);
    result->pool_stats = pool->getStats();

    pool->shutdown();

    std::lock_guard<std::mutex> lock(completion->mutex);
    completion->done = true;
    completion->exit_code = result->finished ? 0 : 2;
    completion->cv.notify_one();
}

}  // namespace

int main(int argc, char* argv[])
{
    BenchmarkOptions options;
    bool show_help = false;
    if (!parseArgs(argc, argv, options, show_help)) {
        printUsage(argv[0]);
        return show_help ? 0 : 1;
    }

    std::cout << "==================================================" << std::endl;
    std::cout << "Connection Pool Benchmark (B2)" << std::endl;
    std::cout << "==================================================" << std::endl;
    std::cout << "Host: " << options.host << ":" << options.port << std::endl;
    std::cout << "Workers: " << options.workers << std::endl;
    std::cout << "Operations per worker: " << options.operations << std::endl;
    std::cout << "Pool min/max: " << options.min_connections << "/" << options.max_connections << std::endl;
    std::cout << "Planned operations: "
              << static_cast<std::int64_t>(options.workers) * options.operations * 2
              << std::endl;
    std::cout << "==================================================" << std::endl;

    Runtime runtime;
    runtime.start();

    auto* scheduler = runtime.getNextIOScheduler();
    if (!scheduler) {
        std::cerr << "Failed to get IO scheduler" << std::endl;
        runtime.stop();
        return 1;
    }

    BenchmarkResult result;
    CompletionState completion;
    scheduleTask(scheduler, runBenchmark(scheduler, &options, &result, &completion));

    {
        std::unique_lock<std::mutex> lock(completion.mutex);
        completion.cv.wait_for(lock, std::chrono::seconds(240), [&]() { return completion.done; });
    }

    runtime.stop();

    if (!completion.done) {
        std::cerr << "Benchmark timeout after 240s" << std::endl;
        return 2;
    }

    if (!result.init_success) {
        std::cerr << "Failed to initialize connection pool: " << result.init_error << std::endl;
        return 1;
    }

    const std::int64_t total = result.success + result.error + result.timeout;

    std::cout << "\n==================================================" << std::endl;
    std::cout << "Benchmark Results" << std::endl;
    std::cout << "==================================================" << std::endl;
    std::cout << "Finished: " << (result.finished ? "yes" : "no (timeout)") << std::endl;
    std::cout << "Duration: " << result.duration_ms << "ms" << std::endl;
    std::cout << "Success: " << result.success << std::endl;
    std::cout << "Error: " << result.error << std::endl;
    std::cout << "Timeout: " << result.timeout << std::endl;
    if (result.duration_ms > 0) {
        const double qps = static_cast<double>(result.success) / (static_cast<double>(result.duration_ms) / 1000.0);
        std::cout << "Ops/sec: " << static_cast<std::int64_t>(qps) << std::endl;
    }
    if (total > 0) {
        const double success_rate = static_cast<double>(result.success) * 100.0 / static_cast<double>(total);
        std::cout << "Success rate: " << success_rate << "%" << std::endl;
    }
    std::cout << "\nPool stats:" << std::endl;
    std::cout << "  total_connections: " << result.pool_stats.total_connections << std::endl;
    std::cout << "  available_connections: " << result.pool_stats.available_connections << std::endl;
    std::cout << "  active_connections: " << result.pool_stats.active_connections << std::endl;
    std::cout << "  total_acquired: " << result.pool_stats.total_acquired << std::endl;
    std::cout << "  total_released: " << result.pool_stats.total_released << std::endl;
    std::cout << "  peak_active_connections: " << result.pool_stats.peak_active_connections << std::endl;
    std::cout << "==================================================" << std::endl;

    return completion.exit_code;
}
