#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <new>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include <galay/cpp/galay-kernel/core/runtime.h>

#include "common/config.h"
#include <galay/cpp/galay-mysql/async/client.h>

using namespace galay::kernel;
using namespace galay::mysql;

namespace
{

namespace alloc_stats
{
std::atomic<uint64_t> g_alloc_count{0};
std::atomic<uint64_t> g_alloc_bytes{0};
std::atomic<uint64_t> g_free_count{0};

struct Snapshot
{
    uint64_t alloc_count = 0;
    uint64_t alloc_bytes = 0;
};

inline Snapshot snapshot()
{
    return Snapshot{
        g_alloc_count.load(std::memory_order_relaxed),
        g_alloc_bytes.load(std::memory_order_relaxed)
    };
}
} // namespace alloc_stats
} // namespace

namespace
{

double percentile(std::vector<uint64_t> samples, double p)
{
    if (samples.empty()) {
        return 0.0;
    }
    if (p < 0.0) p = 0.0;
    if (p > 1.0) p = 1.0;

    std::sort(samples.begin(), samples.end());
    const size_t idx = static_cast<size_t>(p * static_cast<double>(samples.size() - 1));
    return static_cast<double>(samples[idx]) / 1e6;
}

struct BenchmarkState {
    std::atomic<size_t> finished_clients{0};
    std::atomic<uint64_t> success{0};
    std::atomic<uint64_t> failed{0};
    std::atomic<uint64_t> latency_ns{0};
    std::atomic<uint64_t> alloc_count_delta{0};
    std::atomic<uint64_t> alloc_bytes_delta{0};
    std::mutex error_mutex;
    std::string first_error;
    std::mutex latency_mutex;
    std::vector<uint64_t> latencies_ns;

    void recordError(std::string message)
    {
        std::lock_guard<std::mutex> lock(error_mutex);
        if (first_error.empty()) {
            first_error = std::move(message);
        }
    }
};

Task<void> runWorker(IOScheduler* scheduler,
                    BenchmarkState* state,
                    mysql_benchmark::DbBenchmarkConfig cfg)
{
    auto client = AsyncMysqlClientBuilder()
        .scheduler(scheduler)
        .bufferSize(cfg.buffer_size)
        .build();

    auto connect_result = co_await client.connect(cfg.host, cfg.port, cfg.user, cfg.password, cfg.database);
    if (!connect_result || !connect_result->has_value()) {
        state->failed.fetch_add(static_cast<uint64_t>(cfg.queries_per_client), std::memory_order_relaxed);
        if (!connect_result) {
            state->recordError("connect failed: " + connect_result.error().message());
        } else {
            state->recordError("connect failed: awaitable resumed without value");
        }
        state->finished_clients.fetch_add(1, std::memory_order_release);
        co_return;
    }

    for (size_t i = 0; i < cfg.warmup_queries; ++i) {
        auto _ = co_await client.query(cfg.sql);
        (void)_;
    }

    std::vector<uint64_t> local_latencies;
    local_latencies.reserve(cfg.queries_per_client);
    uint64_t local_alloc_count = 0;
    uint64_t local_alloc_bytes = 0;
    const size_t configured_batch_size = cfg.batch_size == 0 ? size_t{1} : cfg.batch_size;
    protocol::MysqlCommandBuilder pipeline_builder_full;
    protocol::MysqlCommandBuilder pipeline_builder_tail;
    std::span<const protocol::MysqlCommandView> pipeline_commands_full;
    std::span<const protocol::MysqlCommandView> pipeline_commands_tail;
    size_t pipeline_tail_size = 0;
    if (cfg.mode == mysql_benchmark::BenchmarkMode::Pipeline) {
        const size_t reserve_per_cmd = protocol::MYSQL_PACKET_HEADER_SIZE + 1 + cfg.sql.size();
        pipeline_builder_full.reserve(configured_batch_size, configured_batch_size * reserve_per_cmd);
        for (size_t i = 0; i < configured_batch_size; ++i) {
            pipeline_builder_full.appendQuery(cfg.sql);
        }
        pipeline_commands_full = pipeline_builder_full.commands();

        pipeline_tail_size = cfg.queries_per_client % configured_batch_size;
        if (pipeline_tail_size > 0) {
            pipeline_builder_tail.reserve(pipeline_tail_size, pipeline_tail_size * reserve_per_cmd);
            for (size_t i = 0; i < pipeline_tail_size; ++i) {
                pipeline_builder_tail.appendQuery(cfg.sql);
            }
            pipeline_commands_tail = pipeline_builder_tail.commands();
        }
    }

    size_t done = 0;
    while (done < cfg.queries_per_client) {
        const bool is_batch = cfg.mode == mysql_benchmark::BenchmarkMode::Batch;
        const bool is_pipeline = cfg.mode == mysql_benchmark::BenchmarkMode::Pipeline;
        const size_t batch_size = (is_batch || is_pipeline)
            ? std::min(configured_batch_size, cfg.queries_per_client - done)
            : 1;

        if (is_batch) {
            auto begin_tx = co_await client.beginTransaction();
            if (!begin_tx || !begin_tx->has_value()) {
                state->failed.fetch_add(static_cast<uint64_t>(batch_size), std::memory_order_relaxed);
                state->recordError("begin transaction failed");
                done += batch_size;
                continue;
            }
        }

        uint64_t batch_elapsed = 0;
        uint64_t batch_alloc_count = 0;
        uint64_t batch_alloc_bytes = 0;
        size_t batch_success = 0;

        if (is_pipeline) {
            const auto command_span =
                (batch_size == configured_batch_size || pipeline_tail_size == 0)
                    ? pipeline_commands_full
                    : pipeline_commands_tail;
            const auto alloc_before = cfg.alloc_stats ? alloc_stats::snapshot() : alloc_stats::Snapshot{};
            const auto started = std::chrono::steady_clock::now();
            auto pipeline_result = co_await client.batch(command_span);
            const auto finished = std::chrono::steady_clock::now();

            batch_elapsed = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(finished - started).count());

            if (cfg.alloc_stats) {
                const auto alloc_after = alloc_stats::snapshot();
                batch_alloc_count += alloc_after.alloc_count - alloc_before.alloc_count;
                batch_alloc_bytes += alloc_after.alloc_bytes - alloc_before.alloc_bytes;
            }

            if (!pipeline_result) {
                state->recordError("pipeline failed: " + pipeline_result.error().message());
            } else if (!pipeline_result->has_value()) {
                state->recordError("pipeline failed: awaitable resumed without value");
            } else if (pipeline_result->value().size() != batch_size) {
                state->recordError("pipeline response count mismatch");
            } else {
                batch_success = batch_size;
            }
        } else {
            for (size_t j = 0; j < batch_size; ++j) {
                const auto alloc_before = cfg.alloc_stats ? alloc_stats::snapshot() : alloc_stats::Snapshot{};
                const auto started = std::chrono::steady_clock::now();
                auto query_result = co_await client.query(cfg.sql);
                const auto finished = std::chrono::steady_clock::now();

                const auto elapsed_ns = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(finished - started).count());
                batch_elapsed += elapsed_ns;

                if (!query_result || !query_result->has_value()) {
                    if (!query_result) {
                        state->recordError("query failed: " + query_result.error().message());
                    } else {
                        state->recordError("query failed: awaitable resumed without value");
                    }
                } else {
                    ++batch_success;
                }

                if (cfg.alloc_stats) {
                    const auto alloc_after = alloc_stats::snapshot();
                    batch_alloc_count += alloc_after.alloc_count - alloc_before.alloc_count;
                    batch_alloc_bytes += alloc_after.alloc_bytes - alloc_before.alloc_bytes;
                }
            }
        }

        if (is_batch) {
            auto end_tx = co_await client.commit();
            if (!end_tx || !end_tx->has_value()) {
                state->recordError("commit failed");
            }
        }

        const uint64_t per_op_elapsed = batch_elapsed / static_cast<uint64_t>(batch_size);
        for (size_t j = 0; j < batch_size; ++j) {
            local_latencies.push_back(per_op_elapsed);
        }

        state->latency_ns.fetch_add(batch_elapsed, std::memory_order_relaxed);
        state->success.fetch_add(static_cast<uint64_t>(batch_success), std::memory_order_relaxed);
        state->failed.fetch_add(static_cast<uint64_t>(batch_size - batch_success), std::memory_order_relaxed);

        local_alloc_count += batch_alloc_count;
        local_alloc_bytes += batch_alloc_bytes;
        done += batch_size;
    }

    auto _ = co_await client.close();
    (void)_;

    state->alloc_count_delta.fetch_add(local_alloc_count, std::memory_order_relaxed);
    state->alloc_bytes_delta.fetch_add(local_alloc_bytes, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(state->latency_mutex);
        state->latencies_ns.insert(state->latencies_ns.end(), local_latencies.begin(), local_latencies.end());
    }

    state->finished_clients.fetch_add(1, std::memory_order_release);
}

void printSummary(const mysql_benchmark::DbBenchmarkConfig& cfg,
                  BenchmarkState& state,
                  std::chrono::steady_clock::time_point started,
                  std::chrono::steady_clock::time_point finished)
{
    const auto elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(finished - started).count();
    const auto elapsed_sec = static_cast<double>(elapsed_ns) / 1e9;

    const uint64_t success = state.success.load(std::memory_order_relaxed);
    const uint64_t failed = state.failed.load(std::memory_order_relaxed);
    const uint64_t total = success + failed;
    const uint64_t latency_ns = state.latency_ns.load(std::memory_order_relaxed);
    const uint64_t alloc_count_delta = state.alloc_count_delta.load(std::memory_order_relaxed);
    const uint64_t alloc_bytes_delta = state.alloc_bytes_delta.load(std::memory_order_relaxed);

    const double qps = elapsed_sec > 0.0 ? static_cast<double>(success) / elapsed_sec : 0.0;
    const double avg_latency_ms = total > 0
        ? (static_cast<double>(latency_ns) / static_cast<double>(total)) / 1e6
        : 0.0;

    std::vector<uint64_t> latencies_copy;
    {
        std::lock_guard<std::mutex> lock(state.latency_mutex);
        latencies_copy = state.latencies_ns;
    }

    std::cout << "\n=== B2 Async Pressure Summary ===\n"
              << "mode: " << mysql_benchmark::modeToString(cfg.mode) << '\n'
              << "clients: " << cfg.clients << '\n'
              << "queries_per_client: " << cfg.queries_per_client << '\n'
              << "total_queries: " << total << '\n'
              << "success: " << success << '\n'
              << "failed: " << failed << '\n'
              << "elapsed_sec: " << elapsed_sec << '\n'
              << "qps: " << qps << '\n'
              << "avg_latency_ms: " << avg_latency_ms << '\n'
              << "p50_latency_ms: " << percentile(latencies_copy, 0.50) << '\n'
              << "p95_latency_ms: " << percentile(latencies_copy, 0.95) << '\n'
              << "p99_latency_ms: " << percentile(latencies_copy, 0.99) << '\n'
              << "max_latency_ms: " << percentile(latencies_copy, 1.0) << std::endl;

    if (cfg.alloc_stats && total > 0) {
        std::cout << "avg_allocs_per_op: "
                  << static_cast<double>(alloc_count_delta) / static_cast<double>(total) << '\n'
                  << "avg_alloc_bytes_per_op: "
                  << static_cast<double>(alloc_bytes_delta) / static_cast<double>(total) << std::endl;
    }

    if (!state.first_error.empty()) {
        std::cout << "first_error: " << state.first_error << std::endl;
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
    std::cout << "Running async pressure benchmark..." << std::endl;

    Runtime runtime;
    runtime.start();

    BenchmarkState state;
    {
        std::lock_guard<std::mutex> lock(state.latency_mutex);
        state.latencies_ns.reserve(cfg.clients * cfg.queries_per_client);
    }

    for (size_t i = 0; i < cfg.clients; ++i) {
        auto* scheduler = runtime.getNextIOScheduler();
        if (scheduler == nullptr) {
            runtime.stop();
            std::cerr << "failed to get IO scheduler" << std::endl;
            return 1;
        }
        if (!scheduleTask(scheduler, runWorker(scheduler, &state, cfg))) {
            runtime.stop();
            std::cerr << "failed to schedule async benchmark worker on IO scheduler" << std::endl;
            return 1;
        }
    }

    const auto started = std::chrono::steady_clock::now();
    const auto deadline = started + std::chrono::seconds(cfg.timeout_seconds);
    while (state.finished_clients.load(std::memory_order_acquire) < cfg.clients &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    const auto finished = std::chrono::steady_clock::now();

    runtime.stop();

    if (state.finished_clients.load(std::memory_order_acquire) < cfg.clients) {
        std::cerr << "benchmark timeout after " << cfg.timeout_seconds << " seconds" << std::endl;
        return 1;
    }

    printSummary(cfg, state, started, finished);
    return state.failed.load(std::memory_order_relaxed) == 0 ? 0 : 1;
}
