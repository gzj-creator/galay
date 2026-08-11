#include "common/config.h"

#include <galay/cpp/galay-postgres/sync/postgres_client.h>

#include <algorithm>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace
{

constexpr size_t kWarmupQueries = 32;

struct BenchmarkState
{
    std::mutex samples_mutex;
    std::mutex error_mutex;
    std::vector<uint64_t> samples_ns;
    std::string first_error;
    std::atomic<uint64_t> response_bytes{0};
    std::atomic<uint64_t> succeeded{0};
    std::atomic<uint64_t> failed{0};
};

void rememberError(BenchmarkState* state, std::string message)
{
    std::lock_guard lock(state->error_mutex);
    if (state->first_error.empty()) {
        state->first_error = std::move(message);
    }
}

uint64_t resultBytes(const galay::postgres::PostgresResultSet& result)
{
    uint64_t bytes = 0;
    for (const auto& row : result.rows()) {
        for (const auto& value : row.values()) {
            if (value.has_value()) {
                bytes += value->size();
            }
        }
    }
    return bytes;
}

void runWorker(const postgres_benchmark::Config* config,
               BenchmarkState* state,
               std::barrier<>* ready_barrier,
               std::barrier<>* start_barrier,
               std::barrier<>* finish_barrier)
{
    galay::postgres::PostgresClient client;
    auto connected = client.connect(config->host,
                                    config->port,
                                    config->user,
                                    config->password,
                                    config->database);
    bool ready = connected.has_value();
    if (!ready) {
        rememberError(state, connected.error().message());
    }

    if (ready) {
        for (size_t index = 0; index < kWarmupQueries; ++index) {
            auto result = client.query(config->sql);
            if (!result) {
                rememberError(state, result.error().message());
                ready = false;
                break;
            }
        }
    }

    ready_barrier->arrive_and_wait();
    start_barrier->arrive_and_wait();
    if (!ready) {
        state->failed.fetch_add(config->queries, std::memory_order_relaxed);
        finish_barrier->arrive_and_wait();
        return;
    }

    std::vector<uint64_t> local_samples;
    local_samples.reserve(config->queries);
    uint64_t local_response_bytes = 0;
    uint64_t local_succeeded = 0;
    uint64_t local_failed = 0;
    for (size_t index = 0; index < config->queries; ++index) {
        const auto started = std::chrono::steady_clock::now();
        auto result = client.query(config->sql);
        const auto finished = std::chrono::steady_clock::now();
        local_samples.push_back(static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(finished - started).count()));
        if (result) {
            ++local_succeeded;
            local_response_bytes += resultBytes(*result);
        } else {
            ++local_failed;
            rememberError(state, result.error().message());
        }
    }

    state->succeeded.fetch_add(local_succeeded, std::memory_order_relaxed);
    state->failed.fetch_add(local_failed, std::memory_order_relaxed);
    state->response_bytes.fetch_add(local_response_bytes, std::memory_order_relaxed);
    finish_barrier->arrive_and_wait();
    {
        std::lock_guard lock(state->samples_mutex);
        state->samples_ns.insert(state->samples_ns.end(),
                                 local_samples.begin(),
                                 local_samples.end());
    }
}

double percentileMs(const std::vector<uint64_t>& sorted_samples, double fraction)
{
    if (sorted_samples.empty()) {
        return 0.0;
    }
    const size_t index = static_cast<size_t>(
        fraction * static_cast<double>(sorted_samples.size() - 1));
    return static_cast<double>(sorted_samples[index]) / 1e6;
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

    BenchmarkState state;
    state.samples_ns.reserve(config.clients * config.queries);
    std::barrier ready_barrier(static_cast<std::ptrdiff_t>(config.clients + 1));
    std::barrier start_barrier(static_cast<std::ptrdiff_t>(config.clients + 1));
    std::barrier finish_barrier(static_cast<std::ptrdiff_t>(config.clients + 1));

    std::vector<std::thread> workers;
    workers.reserve(config.clients);
    for (size_t index = 0; index < config.clients; ++index) {
        workers.emplace_back(runWorker,
                             &config,
                             &state,
                             &ready_barrier,
                             &start_barrier,
                             &finish_barrier);
    }
    ready_barrier.arrive_and_wait();
    const auto started = std::chrono::steady_clock::now();
    start_barrier.arrive_and_wait();
    finish_barrier.arrive_and_wait();
    const auto finished = std::chrono::steady_clock::now();
    for (auto& worker : workers) {
        worker.join();
    }

    std::sort(state.samples_ns.begin(), state.samples_ns.end());
    const uint64_t succeeded = state.succeeded.load(std::memory_order_relaxed);
    const uint64_t failed = state.failed.load(std::memory_order_relaxed);
    const double seconds = std::chrono::duration<double>(finished - started).count();
    const double qps = seconds > 0.0 ? static_cast<double>(succeeded) / seconds : 0.0;

    std::cout << "\n=== Galay PostgreSQL Query Pressure Summary ===\n"
              << "clients: " << config.clients << '\n'
              << "warmup_queries_per_client: " << kWarmupQueries << '\n'
              << "queries_per_client: " << config.queries << '\n'
              << "total_queries: " << succeeded + failed << '\n'
              << "success: " << succeeded << '\n'
              << "failed: " << failed << '\n'
              << "response_bytes: "
              << state.response_bytes.load(std::memory_order_relaxed) << '\n'
              << "elapsed_sec: " << seconds << '\n'
              << "qps: " << qps << '\n'
              << "p50_latency_ms: " << percentileMs(state.samples_ns, 0.50) << '\n'
              << "p95_latency_ms: " << percentileMs(state.samples_ns, 0.95) << '\n'
              << "p99_latency_ms: " << percentileMs(state.samples_ns, 0.99) << '\n'
              << "max_latency_ms: " << percentileMs(state.samples_ns, 1.0) << '\n';
    if (!state.first_error.empty()) {
        std::cout << "first_error: " << state.first_error;
        if (state.first_error.back() != '\n') {
            std::cout << '\n';
        }
    }
    return failed == 0 ? 0 : 1;
}
