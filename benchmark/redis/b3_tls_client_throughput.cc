#include "galay-redis/async/redis_client.h"

#include <galay-kernel/core/runtime.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <optional>
#include <semaphore>
#include <string>
#include <vector>

using namespace galay::kernel;
using namespace galay::redis;

namespace {

struct BenchmarkOptions {
    std::string url = "rediss://localhost:16380/0";
    int clients = 10;
    int operations = 200;
    std::string mode = "normal";
    int batch_size = 100;
    int timeout_ms = 5000;
    size_t buffer_size = 65536;
    std::string ca_cert;
    bool verify_peer = false;
    std::string server_name;
    bool verbose = true;
};

struct ClientResult {
    std::int64_t success = 0;
    std::int64_t error = 0;
    std::int64_t timeout = 0;
    std::vector<std::int64_t> latencies;
};

struct PrebuiltBenchmarkInputs {
    std::vector<std::string> keys;
    std::vector<std::string> values;
};

std::atomic<int> g_completed_clients{0};
std::counting_semaphore<std::numeric_limits<int>::max()> g_completed_sem(0);
std::vector<ClientResult> g_client_results;

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
              << " [--url rediss://host:port/db] [-c clients] [-n operations] "
                 "[-m normal|pipeline] [-b batch_size] [--timeout-ms N] [--buffer-size bytes] "
                 "[--ca-cert path] [--verify-peer 0|1] [--server-name host] [-q]"
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
        if (arg == "--url") {
            options.url = value;
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
            if (!parseInt(value, options.timeout_ms) || options.timeout_ms <= 0) {
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
        if (arg == "--ca-cert") {
            options.ca_cert = value;
            continue;
        }
        if (arg == "--verify-peer") {
            if (value == "1" || value == "true") {
                options.verify_peer = true;
            } else if (value == "0" || value == "false") {
                options.verify_peer = false;
            } else {
                std::cerr << "Invalid verify-peer: " << value << std::endl;
                return false;
            }
            continue;
        }
        if (arg == "--server-name") {
            options.server_name = value;
            continue;
        }

        std::cerr << "Unknown argument: " << arg << std::endl;
        return false;
    }

    if (options.mode != "normal" && options.mode != "pipeline") {
        std::cerr << "Invalid mode: " << options.mode << ", expected normal|pipeline" << std::endl;
        return false;
    }
    return true;
}

RedissClientConfig makeTlsConfig(const BenchmarkOptions& options)
{
    RedissClientConfig config;
    config.ca_path = options.ca_cert;
    config.verify_peer = options.verify_peer;
    config.server_name = options.server_name;
    return config;
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

Coroutine benchmarkNormal(IOScheduler* scheduler, const BenchmarkOptions* options, int client_id)
{
    auto client = RedissClientBuilder()
                      .scheduler(scheduler)
                      .bufferSize(options->buffer_size)
                      .tlsConfig(makeTlsConfig(*options))
                      .build();

    RedisCommandBuilder command_builder;
    std::int64_t local_success = 0;
    std::int64_t local_error = 0;
    std::int64_t local_timeout = 0;
    std::vector<std::int64_t> local_latencies;
    local_latencies.reserve(static_cast<size_t>(options->operations) * 2U);
    const auto request_timeout = std::chrono::milliseconds(options->timeout_ms);

    auto connect_result = co_await client.connect(options->url).timeout(request_timeout);
    if (!connect_result) {
        local_error += static_cast<std::int64_t>(options->operations) * 2;
        storeClientResult(client_id, local_success, local_error, local_timeout, std::move(local_latencies));
        markClientCompleted();
        co_return;
    }

    auto inputs = buildInputs("bench:rediss:normal", client_id, options->operations);

    const auto start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < options->operations; ++i) {
        const auto& key = inputs.keys[static_cast<size_t>(i)];
        const auto& value = inputs.values[static_cast<size_t>(i)];

        const auto set_begin = std::chrono::high_resolution_clock::now();
        auto set_result = co_await client.command(command_builder.set(key, value)).timeout(request_timeout);
        const auto set_end = std::chrono::high_resolution_clock::now();
        local_latencies.push_back(std::chrono::duration_cast<std::chrono::microseconds>(set_end - set_begin).count());
        countSingleResult(set_result, local_success, local_error, local_timeout);

        const auto get_begin = std::chrono::high_resolution_clock::now();
        auto get_result = co_await client.command(command_builder.get(key)).timeout(request_timeout);
        const auto get_end = std::chrono::high_resolution_clock::now();
        local_latencies.push_back(std::chrono::duration_cast<std::chrono::microseconds>(get_end - get_begin).count());
        countSingleResult(get_result, local_success, local_error, local_timeout);
    }
    const auto end = std::chrono::high_resolution_clock::now();

    (void)co_await client.close();
    storeClientResult(client_id, local_success, local_error, local_timeout, std::move(local_latencies));

    if (options->verbose) {
        const auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
        std::cout << "TLS client " << client_id << " finished normal mode in "
                  << duration.count() << "ms" << std::endl;
    }

    markClientCompleted();
}

Coroutine benchmarkPipeline(IOScheduler* scheduler, const BenchmarkOptions* options, int client_id)
{
    auto client = RedissClientBuilder()
                      .scheduler(scheduler)
                      .bufferSize(options->buffer_size)
                      .tlsConfig(makeTlsConfig(*options))
                      .build();

    std::int64_t local_success = 0;
    std::int64_t local_error = 0;
    std::int64_t local_timeout = 0;
    std::vector<std::int64_t> local_latencies;
    const size_t latency_reserve = static_cast<size_t>((options->operations + options->batch_size - 1) / options->batch_size);
    local_latencies.reserve(latency_reserve);
    const auto request_timeout = std::chrono::milliseconds(options->timeout_ms);

    auto connect_result = co_await client.connect(options->url).timeout(request_timeout);
    if (!connect_result) {
        local_error += options->operations;
        storeClientResult(client_id, local_success, local_error, local_timeout, std::move(local_latencies));
        markClientCompleted();
        co_return;
    }

    auto inputs = buildInputs("bench:rediss:pipeline", client_id, options->operations);

    const auto start = std::chrono::high_resolution_clock::now();
    int offset = 0;
    RedisCommandBuilder builder;
    builder.reserve(
        static_cast<size_t>(options->batch_size),
        static_cast<size_t>(options->batch_size) * 2U,
        static_cast<size_t>(options->batch_size) * 96U);

    while (offset < options->operations) {
        const int current_batch = std::min(options->batch_size, options->operations - offset);
        const auto call_begin = std::chrono::high_resolution_clock::now();

        builder.clear();
        for (int i = 0; i < current_batch; ++i) {
            const auto index = static_cast<size_t>(offset + i);
            const auto& key = inputs.keys[index];
            const auto& value = inputs.values[index];
            builder.append("SET", std::array<std::string_view, 2>{key, value});
        }

        auto pipeline_result = co_await client.batch(builder.commands()).timeout(request_timeout);
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
        std::cout << "TLS client " << client_id << " finished pipeline mode in "
                  << duration.count() << "ms" << std::endl;
    }

    markClientCompleted();
}

} // namespace

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
    g_client_results.clear();
    g_client_results.resize(static_cast<size_t>(options.clients));

    std::cout << "==================================================" << std::endl;
    std::cout << "Rediss Client Benchmark (B3)" << std::endl;
    std::cout << "==================================================" << std::endl;
    std::cout << "URL: " << options.url << std::endl;
    std::cout << "Clients: " << options.clients << std::endl;
    std::cout << "Operations per client: " << options.operations << std::endl;
    std::cout << "Mode: " << options.mode << std::endl;
    if (options.mode == "pipeline") {
        std::cout << "Batch size: " << options.batch_size << std::endl;
    }
    std::cout << "Timeout (ms): " << options.timeout_ms << std::endl;
    std::cout << "Client buffer size: " << options.buffer_size << std::endl;
    std::cout << "Verify peer: " << (options.verify_peer ? "on" : "off") << std::endl;
    const std::int64_t planned_ops =
        options.mode == "pipeline"
            ? static_cast<std::int64_t>(options.clients) * options.operations
            : static_cast<std::int64_t>(options.clients) * options.operations * 2;
    std::cout << "Planned operations: " << planned_ops << std::endl;
    std::cout << "==================================================" << std::endl;

    Runtime runtime;
    runtime.start();

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
    runtime.stop();

    const auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(bench_end - bench_start).count();
    std::int64_t success = 0;
    std::int64_t error = 0;
    std::int64_t timeout = 0;

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
    }
    std::cout << "==================================================" << std::endl;

    return finished ? 0 : 2;
}
