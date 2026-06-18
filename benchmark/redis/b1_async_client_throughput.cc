#include "common/config.h"
#include "galay-redis/async/redis_client.h"
#include <galay-kernel/core/runtime.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <iostream>
#include <limits>
#include <new>
#include <semaphore>
#include <string>
#include <vector>

using namespace galay::kernel;
using namespace galay::redis;

std::atomic<bool> g_alloc_tracking_enabled{false};
std::atomic<std::uint64_t> g_alloc_calls{0};
std::atomic<std::uint64_t> g_alloc_bytes{0};

void* operator new(std::size_t size)
{
    const std::size_t actual_size = size == 0 ? 1 : size;
    if (void* p = std::malloc(actual_size)) {
        if (g_alloc_tracking_enabled.load(std::memory_order_relaxed)) {
            g_alloc_calls.fetch_add(1, std::memory_order_relaxed);
            g_alloc_bytes.fetch_add(actual_size, std::memory_order_relaxed);
        }
        return p;
    }
    throw std::bad_alloc();
}

void* operator new[](std::size_t size)
{
    return ::operator new(size);
}

void* operator new(std::size_t size, const std::nothrow_t&) noexcept
{
    try {
        return ::operator new(size);
    } catch (...) {
        return nullptr;
    }
}

void* operator new[](std::size_t size, const std::nothrow_t&) noexcept
{
    try {
        return ::operator new[](size);
    } catch (...) {
        return nullptr;
    }
}

void operator delete(void* ptr) noexcept
{
    std::free(ptr);
}

void operator delete[](void* ptr) noexcept
{
    std::free(ptr);
}

void operator delete(void* ptr, std::size_t) noexcept
{
    std::free(ptr);
}

void operator delete[](void* ptr, std::size_t) noexcept
{
    std::free(ptr);
}

namespace {

struct BenchmarkOptions {
    std::string host = galay::redis::example::kDefaultRedisHost;
    int port = galay::redis::example::kDefaultRedisPort;
    int clients = 10;
    int operations = 100;
    std::string mode = "normal";
    int batch_size = 100;
    int timeout_ms = -1;
    size_t buffer_size = 65536;
    bool track_alloc = false;
    bool verbose = true;
};

std::atomic<int> g_completed_clients{0};
std::counting_semaphore<std::numeric_limits<int>::max()> g_completed_sem(0);
struct ClientResult {
    std::int64_t success = 0;
    std::int64_t error = 0;
    std::int64_t timeout = 0;
    std::vector<std::int64_t> latencies;
};
std::vector<ClientResult> g_client_results;

struct PrebuiltBenchmarkInputs {
    std::vector<std::string> keys;
    std::vector<std::string> values;
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

bool parseSize(const std::string& text, size_t& value)
{
    try {
        size_t used = 0;
        const auto parsed = std::stoull(text, &used);
        if (used != text.size()) return false;
        value = static_cast<size_t>(parsed);
        return true;
    } catch (...) {
        return false;
    }
}

void printUsage(const char* program)
{
    std::cout << "Usage: " << program
              << " [-h host] [-p port] [-c clients] [-n operations] "
                 "[-m normal|normal-batch|pipeline] [-b batch_size] "
                 "[--timeout-ms -1|N] [--buffer-size bytes] "
                 "[--track-alloc] [-q]"
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
        if (arg == "--track-alloc") {
            options.track_alloc = true;
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
        if (arg == "-c" || arg == "--clients") {
            if (!parseInt(value, options.clients) || options.clients <= 0) {
                std::cerr << "Invalid clients: " << value << std::endl;
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
        if (arg == "-m" || arg == "--mode") {
            options.mode = value;
            continue;
        }
        if (arg == "-b" || arg == "--batch-size") {
            if (!parseInt(value, options.batch_size) || options.batch_size <= 0) {
                std::cerr << "Invalid batch-size: " << value << std::endl;
                return false;
            }
            continue;
        }
        if (arg == "--timeout-ms") {
            if (!parseInt(value, options.timeout_ms) || options.timeout_ms < -1) {
                std::cerr << "Invalid timeout-ms: " << value << std::endl;
                return false;
            }
            continue;
        }
        if (arg == "--buffer-size") {
            if (!parseSize(value, options.buffer_size) || options.buffer_size == 0) {
                std::cerr << "Invalid buffer-size: " << value << std::endl;
                return false;
            }
            continue;
        }
        std::cerr << "Unknown argument: " << arg << std::endl;
        return false;
    }

    if (options.mode != "normal" && options.mode != "normal-batch" && options.mode != "pipeline") {
        std::cerr << "Invalid mode: " << options.mode << ", expected normal|normal-batch|pipeline" << std::endl;
        return false;
    }
    if ((options.mode == "pipeline" || options.mode == "normal-batch") && options.batch_size <= 0) {
        std::cerr << "batch-size must be > 0 in pipeline/normal-batch mode" << std::endl;
        return false;
    }
    return true;
}

void markClientCompleted()
{
    g_completed_clients.fetch_add(1, std::memory_order_release);
    g_completed_sem.release();
}

void storeClientResult(int client_id,
                       std::int64_t success,
                       std::int64_t error,
                       std::int64_t timeout,
                       std::vector<std::int64_t>&& local_latencies)
{
    if (client_id < 0) {
        return;
    }
    const size_t index = static_cast<size_t>(client_id);
    if (index >= g_client_results.size()) {
        return;
    }
    auto& slot = g_client_results[index];
    slot.success = success;
    slot.error = error;
    slot.timeout = timeout;
    slot.latencies = std::move(local_latencies);
}

std::int64_t percentile(const std::vector<std::int64_t>& sorted, double p)
{
    if (sorted.empty()) return 0;
    const double rank = p * static_cast<double>(sorted.size() - 1);
    const size_t idx = static_cast<size_t>(std::ceil(rank));
    return sorted[std::min(idx, sorted.size() - 1)];
}

PrebuiltBenchmarkInputs buildInputs(std::string_view prefix, int client_id, int operations)
{
    PrebuiltBenchmarkInputs inputs;
    inputs.keys.reserve(static_cast<size_t>(operations));
    inputs.values.reserve(static_cast<size_t>(operations));
    for (int i = 0; i < operations; ++i) {
        inputs.keys.push_back(
            std::string(prefix) + ":" + std::to_string(client_id) + ":" + std::to_string(i));
        inputs.values.push_back("value_" + std::to_string(i));
    }
    return inputs;
}

template <typename T>
void countSingleResult(const T& result, std::int64_t& success, std::int64_t& error, std::int64_t& timeout)
{
    if (result && result.value()) {
        ++success;
        return;
    }

    if (!result) {
        if (result.error().type() == REDIS_ERROR_TYPE_TIMEOUT_ERROR) {
            ++timeout;
        } else {
            ++error;
        }
        return;
    }

    ++error;
}

template <typename T>
void countBatchResult(
    const T& result,
    std::int64_t count,
    std::int64_t& success,
    std::int64_t& error,
    std::int64_t& timeout)
{
    if (result && result.value()) {
        success += count;
        return;
    }

    if (!result) {
        if (result.error().type() == REDIS_ERROR_TYPE_TIMEOUT_ERROR) {
            timeout += count;
        } else {
            error += count;
        }
        return;
    }

    error += count;
}

Task<void> benchmarkNormal(IOScheduler* scheduler, const BenchmarkOptions* options, int client_id)
{
    auto client = RedisClientBuilder().scheduler(scheduler).bufferSize(options->buffer_size).build();
    protocol::RespEncoder encoder;
    std::string set_encoded;
    std::string get_encoded;
    std::int64_t local_success = 0;
    std::int64_t local_error = 0;
    std::int64_t local_timeout = 0;
    std::vector<std::int64_t> local_latencies;
    local_latencies.reserve(static_cast<size_t>(options->operations) * 2U);
    const auto request_timeout = std::chrono::milliseconds(options->timeout_ms);

    std::expected<void, RedisError> connect_result;
    if (options->timeout_ms >= 0) {
        connect_result = co_await client.connect(options->host, options->port).timeout(request_timeout);
    } else {
        connect_result = co_await client.connect(options->host, options->port);
    }
    if (!connect_result) {
        local_error += static_cast<std::int64_t>(options->operations) * 2;
        storeClientResult(client_id, local_success, local_error, local_timeout, std::move(local_latencies));
        markClientCompleted();
        co_return;
    }

    auto inputs = buildInputs("bench:normal", client_id, options->operations);
    auto encodeSet = [&](std::string_view key, std::string_view value) -> RedisBorrowedCommand {
        const std::array<std::string_view, 2> args{key, value};
        set_encoded.clear();
        set_encoded.reserve(encoder.estimateCommandBytes("SET", args));
        encoder.appendCommandFast(set_encoded, "SET", args);
        return RedisBorrowedCommand(set_encoded, 1);
    };
    auto encodeGet = [&](std::string_view key) -> RedisBorrowedCommand {
        const std::array<std::string_view, 1> args{key};
        get_encoded.clear();
        get_encoded.reserve(encoder.estimateCommandBytes("GET", args));
        encoder.appendCommandFast(get_encoded, "GET", args);
        return RedisBorrowedCommand(get_encoded, 1);
    };

    const auto start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < options->operations; ++i) {
        const auto& key = inputs.keys[static_cast<size_t>(i)];
        const auto& value = inputs.values[static_cast<size_t>(i)];

        const auto set_begin = std::chrono::high_resolution_clock::now();
        RedisBorrowedCommand set_packet = encodeSet(key, value);
        std::expected<std::optional<std::vector<RedisValue>>, RedisError> set_result;
        if (options->timeout_ms >= 0) {
            set_result = co_await client.commandBorrowed(set_packet).timeout(request_timeout);
        } else {
            set_result = co_await client.commandBorrowed(set_packet);
        }
        const auto set_end = std::chrono::high_resolution_clock::now();
        local_latencies.push_back(std::chrono::duration_cast<std::chrono::microseconds>(set_end - set_begin).count());
        countSingleResult(set_result, local_success, local_error, local_timeout);

        const auto get_begin = std::chrono::high_resolution_clock::now();
        RedisBorrowedCommand get_packet = encodeGet(key);
        std::expected<std::optional<std::vector<RedisValue>>, RedisError> get_result;
        if (options->timeout_ms >= 0) {
            get_result = co_await client.commandBorrowed(get_packet).timeout(request_timeout);
        } else {
            get_result = co_await client.commandBorrowed(get_packet);
        }
        const auto get_end = std::chrono::high_resolution_clock::now();
        local_latencies.push_back(std::chrono::duration_cast<std::chrono::microseconds>(get_end - get_begin).count());
        countSingleResult(get_result, local_success, local_error, local_timeout);
    }
    const auto end = std::chrono::high_resolution_clock::now();

    (void)co_await client.close();

    storeClientResult(client_id, local_success, local_error, local_timeout, std::move(local_latencies));

    if (options->verbose) {
        const auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
        std::cout << "Client " << client_id << " finished normal mode in "
                  << duration.count() << "ms" << std::endl;
    }

    markClientCompleted();
}

Task<void> benchmarkPipeline(IOScheduler* scheduler, const BenchmarkOptions* options, int client_id)
{
    auto client = RedisClientBuilder().scheduler(scheduler).bufferSize(options->buffer_size).build();
    std::int64_t local_success = 0;
    std::int64_t local_error = 0;
    std::int64_t local_timeout = 0;
    std::vector<std::int64_t> local_latencies;
    const size_t latency_reserve = static_cast<size_t>((options->operations + options->batch_size - 1) / options->batch_size);
    local_latencies.reserve(latency_reserve);
    const auto request_timeout = std::chrono::milliseconds(options->timeout_ms);

    std::expected<void, RedisError> connect_result;
    if (options->timeout_ms >= 0) {
        connect_result = co_await client.connect(options->host, options->port).timeout(request_timeout);
    } else {
        connect_result = co_await client.connect(options->host, options->port);
    }
    if (!connect_result) {
        local_error += options->operations;
        storeClientResult(client_id, local_success, local_error, local_timeout, std::move(local_latencies));
        markClientCompleted();
        co_return;
    }

    auto inputs = buildInputs("bench:pipeline", client_id, options->operations);

    const auto start = std::chrono::high_resolution_clock::now();
    int offset = 0;
    RedisCommandBuilder builder;
    builder.reserve(
        static_cast<size_t>(options->batch_size),
        static_cast<size_t>(options->batch_size) * 2U,
        static_cast<size_t>(options->batch_size) * 96U);

    while (offset < options->operations) {
        const int current_batch = std::min(options->batch_size, options->operations - offset);
        std::expected<std::optional<std::vector<RedisValue>>, RedisError> pipeline_result;
        const auto call_begin = std::chrono::high_resolution_clock::now();

        builder.clear();
        for (int i = 0; i < current_batch; ++i) {
            const auto index = static_cast<size_t>(offset + i);
            const auto& key = inputs.keys[index];
            const auto& value = inputs.values[index];
            builder.append("SET", std::array<std::string_view, 2>{key, value});
        }
        if (options->timeout_ms >= 0) {
            pipeline_result = co_await client.batchBorrowed(builder.encoded(), builder.size()).timeout(request_timeout);
        } else {
            pipeline_result = co_await client.batchBorrowed(builder.encoded(), builder.size());
        }
        const auto call_end = std::chrono::high_resolution_clock::now();
        local_latencies.push_back(std::chrono::duration_cast<std::chrono::microseconds>(call_end - call_begin).count());

        countBatchResult(
            pipeline_result,
            static_cast<std::int64_t>(current_batch),
            local_success,
            local_error,
            local_timeout);
        offset += current_batch;
    }
    const auto end = std::chrono::high_resolution_clock::now();

    (void)co_await client.close();

    storeClientResult(client_id, local_success, local_error, local_timeout, std::move(local_latencies));

    if (options->verbose) {
        const auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
        std::cout << "Client " << client_id << " finished pipeline mode in "
                  << duration.count() << "ms" << std::endl;
    }

    markClientCompleted();
}

Task<void> benchmarkNormalBatch(IOScheduler* scheduler, const BenchmarkOptions* options, int client_id)
{
    auto client = RedisClientBuilder().scheduler(scheduler).bufferSize(options->buffer_size).build();
    std::int64_t local_success = 0;
    std::int64_t local_error = 0;
    std::int64_t local_timeout = 0;
    std::vector<std::int64_t> local_latencies;
    const size_t latency_reserve = static_cast<size_t>((options->operations + options->batch_size - 1) / options->batch_size);
    local_latencies.reserve(latency_reserve);
    const auto request_timeout = std::chrono::milliseconds(options->timeout_ms);

    std::expected<void, RedisError> connect_result;
    if (options->timeout_ms >= 0) {
        connect_result = co_await client.connect(options->host, options->port).timeout(request_timeout);
    } else {
        connect_result = co_await client.connect(options->host, options->port);
    }
    if (!connect_result) {
        local_error += static_cast<std::int64_t>(options->operations) * 2;
        storeClientResult(client_id, local_success, local_error, local_timeout, std::move(local_latencies));
        markClientCompleted();
        co_return;
    }

    const auto start = std::chrono::high_resolution_clock::now();
    int offset = 0;
    RedisCommandBuilder builder;
    builder.reserve(
        static_cast<size_t>(options->batch_size) * 2U,
        static_cast<size_t>(options->batch_size) * 3U,
        static_cast<size_t>(options->batch_size) * 160U);

    while (offset < options->operations) {
        const int current_batch = std::min(options->batch_size, options->operations - offset);
        std::expected<std::optional<std::vector<RedisValue>>, RedisError> batch_result;
        const auto call_begin = std::chrono::high_resolution_clock::now();

        builder.clear();
        for (int i = 0; i < current_batch; ++i) {
            const int op_index = offset + i;
            const std::string key = "bench:normal-batch:" + std::to_string(client_id) + ":" + std::to_string(op_index);
            const std::string value = "value_" + std::to_string(op_index);
            builder.append("SET", std::array<std::string_view, 2>{key, value});
            builder.append("GET", std::array<std::string_view, 1>{key});
        }
        if (options->timeout_ms >= 0) {
            batch_result = co_await client.batch(builder.commands()).timeout(request_timeout);
        } else {
            batch_result = co_await client.batch(builder.commands());
        }
        const auto call_end = std::chrono::high_resolution_clock::now();
        local_latencies.push_back(std::chrono::duration_cast<std::chrono::microseconds>(call_end - call_begin).count());

        countBatchResult(
            batch_result,
            static_cast<std::int64_t>(current_batch) * 2,
            local_success,
            local_error,
            local_timeout);
        offset += current_batch;
    }
    const auto end = std::chrono::high_resolution_clock::now();

    (void)co_await client.close();

    storeClientResult(client_id, local_success, local_error, local_timeout, std::move(local_latencies));

    if (options->verbose) {
        const auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
        std::cout << "Client " << client_id << " finished normal-batch mode in "
                  << duration.count() << "ms" << std::endl;
    }

    markClientCompleted();
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

    g_completed_clients.store(0, std::memory_order_relaxed);
    while (g_completed_sem.try_acquire()) {
    }
    g_alloc_tracking_enabled.store(false, std::memory_order_relaxed);
    g_alloc_calls.store(0, std::memory_order_relaxed);
    g_alloc_bytes.store(0, std::memory_order_relaxed);
    g_client_results.clear();
    g_client_results.resize(static_cast<size_t>(options.clients));

    std::cout << "==================================================" << std::endl;
    std::cout << "Redis Client Benchmark (B1)" << std::endl;
    std::cout << "==================================================" << std::endl;
    std::cout << "Host: " << options.host << ":" << options.port << std::endl;
    std::cout << "Clients: " << options.clients << std::endl;
    std::cout << "Operations per client: " << options.operations << std::endl;
    std::cout << "Mode: " << options.mode << std::endl;
    if (options.mode == "pipeline" || options.mode == "normal-batch") {
        std::cout << "Batch size: " << options.batch_size << std::endl;
    }
    std::cout << "Timeout (ms): " << options.timeout_ms << std::endl;
    std::cout << "Client buffer size: " << options.buffer_size << std::endl;
    std::cout << "Allocation tracking: " << (options.track_alloc ? "on" : "off") << std::endl;
    const std::int64_t planned_ops =
        options.mode == "pipeline"
            ? static_cast<std::int64_t>(options.clients) * options.operations
            : static_cast<std::int64_t>(options.clients) * options.operations * 2;
    std::cout << "Planned operations: " << planned_ops << std::endl;
    std::cout << "==================================================" << std::endl;

    Runtime runtime;
    runtime.start();

    if (options.track_alloc) {
        g_alloc_calls.store(0, std::memory_order_relaxed);
        g_alloc_bytes.store(0, std::memory_order_relaxed);
        g_alloc_tracking_enabled.store(true, std::memory_order_relaxed);
    }
    const auto bench_start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < options.clients; ++i) {
        auto* scheduler = runtime.getNextIOScheduler();
        if (!scheduler) {
            std::cerr << "Failed to get IO scheduler for client " << i << std::endl;
            runtime.stop();
            return 1;
        }
        if (options.mode == "pipeline") {
            scheduleTask(scheduler, benchmarkPipeline(scheduler, &options, i));
        } else if (options.mode == "normal-batch") {
            scheduleTask(scheduler, benchmarkNormalBatch(scheduler, &options, i));
        } else {
            scheduleTask(scheduler, benchmarkNormal(scheduler, &options, i));
        }
    }

    const auto wait_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(180);
    int acquired_completions = 0;
    for (; acquired_completions < options.clients; ++acquired_completions) {
        if (!g_completed_sem.try_acquire_until(wait_deadline)) {
            break;
        }
    }
    const bool finished = acquired_completions >= options.clients;
    const int completed_clients_snapshot = g_completed_clients.load(std::memory_order_acquire);

    const auto bench_end = std::chrono::high_resolution_clock::now();
    g_alloc_tracking_enabled.store(false, std::memory_order_relaxed);
    runtime.stop();

    const auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(bench_end - bench_start).count();
    std::int64_t success = 0;
    std::int64_t error = 0;
    std::int64_t timeout = 0;
    const std::uint64_t alloc_calls = g_alloc_calls.load(std::memory_order_relaxed);
    const std::uint64_t alloc_bytes = g_alloc_bytes.load(std::memory_order_relaxed);

    std::vector<std::int64_t> latencies;
    size_t total_latency_samples = 0;
    for (const auto& result : g_client_results) {
        success += result.success;
        error += result.error;
        timeout += result.timeout;
        total_latency_samples += result.latencies.size();
    }
    latencies.reserve(total_latency_samples);
    for (auto& result : g_client_results) {
        latencies.insert(
            latencies.end(),
            std::make_move_iterator(result.latencies.begin()),
            std::make_move_iterator(result.latencies.end()));
    }
    const std::int64_t total = success + error + timeout;
    std::sort(latencies.begin(), latencies.end());

    std::cout << "\n==================================================" << std::endl;
    std::cout << "Benchmark Results" << std::endl;
    std::cout << "==================================================" << std::endl;
    std::cout << "Finished: " << (finished ? "yes" : "no (timeout)") << std::endl;
    std::cout << "Completed clients: " << completed_clients_snapshot << "/" << options.clients << std::endl;
    std::cout << "Duration: " << duration_ms << "ms" << std::endl;
    std::cout << "Success: " << success << std::endl;
    std::cout << "Error: " << error << std::endl;
    std::cout << "Timeout: " << timeout << std::endl;
    if (duration_ms > 0) {
        const double qps = static_cast<double>(success) / (static_cast<double>(duration_ms) / 1000.0);
        std::cout << "Ops/sec: " << static_cast<std::int64_t>(qps) << std::endl;
    }
    if (!latencies.empty()) {
        std::cout << "Requests measured: " << latencies.size() << std::endl;
        std::cout << "Request latency p50 (us): " << percentile(latencies, 0.50) << std::endl;
        std::cout << "Request latency p99 (us): " << percentile(latencies, 0.99) << std::endl;
    }
    if (total > 0) {
        const double success_rate = static_cast<double>(success) * 100.0 / static_cast<double>(total);
        std::cout << "Success rate: " << success_rate << "%" << std::endl;
        if (options.track_alloc) {
            const double alloc_per_op = static_cast<double>(alloc_calls) / static_cast<double>(total);
            const double bytes_per_op = static_cast<double>(alloc_bytes) / static_cast<double>(total);
            std::cout << "Alloc calls/op: " << alloc_per_op << std::endl;
            std::cout << "Alloc bytes/op: " << bytes_per_op << std::endl;
        }
    }
    std::cout << "==================================================" << std::endl;

    return finished ? 0 : 2;
}
