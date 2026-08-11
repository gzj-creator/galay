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
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace galay::postgres;

namespace
{

constexpr size_t kWarmupQueries = 32;

struct BenchmarkState
{
    std::mutex samples_mutex;
    std::vector<uint64_t> samples_ns;
    std::atomic<uint64_t> succeeded{0};
    std::atomic<uint64_t> failed{0};
    std::atomic<uint64_t> checksum{0};
};

void runWorker(const postgres_benchmark::Config* config,
               BenchmarkState* state,
               std::barrier<>* ready_barrier,
               std::barrier<>* start_barrier,
               std::barrier<>* finish_barrier)
{
    PostgresClient client;
    auto connected = client.connect(config->host, config->port, config->user,
                                    config->password, config->database);
    bool ready = connected.has_value();
    if (ready) {
        auto prepared = client.prepare("galay_benchmark", "SELECT $1::bigint");
        ready = prepared.has_value();
    }
    if (ready) {
        for (size_t index = 0; index < kWarmupQueries; ++index) {
            const std::vector<std::optional<std::string>> parameters{std::to_string(index)};
            auto result = client.execute("galay_benchmark", parameters);
            if (!result || result->rowCount() == 0) {
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

    uint64_t local_succeeded = 0;
    uint64_t local_failed = 0;
    uint64_t local_checksum = 0;
    std::vector<uint64_t> local_samples;
    local_samples.reserve(config->queries);
    for (size_t iteration = 0; iteration < config->queries; ++iteration) {
        const std::vector<std::optional<std::string>> parameters{std::to_string(iteration)};
        const auto started = std::chrono::steady_clock::now();
        auto result = client.execute("galay_benchmark", parameters);
        const auto finished = std::chrono::steady_clock::now();
        local_samples.push_back(static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(finished - started).count()));
        if (!result || result->rowCount() == 0) {
            ++local_failed;
            continue;
        }
        ++local_succeeded;
        local_checksum += result->row(0).getUint64(0);
    }

    state->succeeded.fetch_add(local_succeeded, std::memory_order_relaxed);
    state->failed.fetch_add(local_failed, std::memory_order_relaxed);
    state->checksum.fetch_add(local_checksum, std::memory_order_relaxed);
    finish_barrier->arrive_and_wait();
    {
        std::lock_guard lock(state->samples_mutex);
        state->samples_ns.insert(state->samples_ns.end(),
                                 local_samples.begin(),
                                 local_samples.end());
    }
    auto closed = client.closePrepared("galay_benchmark");
    if (!closed) {
        state->failed.fetch_add(1, std::memory_order_relaxed);
    }
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
    const auto percentile_ms = [&state](double fraction) {
        if (state.samples_ns.empty()) {
            return 0.0;
        }
        const size_t index = static_cast<size_t>(
            fraction * static_cast<double>(state.samples_ns.size() - 1));
        return static_cast<double>(state.samples_ns[index]) / 1e6;
    };
    const uint64_t succeeded = state.succeeded.load(std::memory_order_relaxed);
    const uint64_t failed = state.failed.load(std::memory_order_relaxed);
    const double seconds = std::chrono::duration<double>(finished - started).count();
    std::cout << "clients=" << config.clients
              << " warmup_queries_per_client=" << kWarmupQueries
              << " total_queries=" << succeeded + failed
              << " success=" << succeeded
              << " failed=" << failed
              << " checksum=" << state.checksum.load(std::memory_order_relaxed)
              << " elapsed_seconds=" << seconds
              << " executes_per_second="
              << (seconds > 0.0 ? static_cast<double>(succeeded) / seconds : 0.0)
              << " p50_latency_ms=" << percentile_ms(0.50)
              << " p95_latency_ms=" << percentile_ms(0.95)
              << " p99_latency_ms=" << percentile_ms(0.99)
              << " max_latency_ms=" << percentile_ms(1.0)
              << '\n';
    return failed == 0 ? 0 : 1;
}
