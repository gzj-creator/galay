#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <vector>

#include "common/config.h"
#include <galay/cpp/galay-mysql/sync/mysql_client.h>

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

void* operator new(std::size_t size)
{
    if (size == 0) {
        size = 1;
    }
    if (void* ptr = std::malloc(size)) {
        alloc_stats::g_alloc_count.fetch_add(1, std::memory_order_relaxed);
        alloc_stats::g_alloc_bytes.fetch_add(static_cast<uint64_t>(size), std::memory_order_relaxed);
        return ptr;
    }
    throw std::bad_alloc();
}

void* operator new[](std::size_t size)
{
    return ::operator new(size);
}

void operator delete(void* ptr) noexcept
{
    if (ptr != nullptr) {
        alloc_stats::g_free_count.fetch_add(1, std::memory_order_relaxed);
    }
    std::free(ptr);
}

void operator delete[](void* ptr) noexcept
{
    ::operator delete(ptr);
}

void operator delete(void* ptr, std::size_t) noexcept
{
    ::operator delete(ptr);
}

void operator delete[](void* ptr, std::size_t) noexcept
{
    ::operator delete[](ptr);
}

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

bool runSingleQuery(MysqlClient& client,
                const mysql_benchmark::DbBenchmarkConfig& cfg,
                uint64_t& elapsed_ns,
                uint64_t& alloc_count_delta,
                uint64_t& alloc_bytes_delta)
{
    const auto alloc_before = cfg.alloc_stats ? alloc_stats::snapshot() : alloc_stats::Snapshot{};
    const auto started = std::chrono::steady_clock::now();
    auto query_result = client.query(cfg.sql);
    const auto finished = std::chrono::steady_clock::now();

    elapsed_ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(finished - started).count());

    if (cfg.alloc_stats) {
        const auto alloc_after = alloc_stats::snapshot();
        alloc_count_delta = alloc_after.alloc_count - alloc_before.alloc_count;
        alloc_bytes_delta = alloc_after.alloc_bytes - alloc_before.alloc_bytes;
    } else {
        alloc_count_delta = 0;
        alloc_bytes_delta = 0;
    }

    return query_result.has_value();
}

void runWorker(const mysql_benchmark::DbBenchmarkConfig& cfg, BenchmarkState* state)
{
    MysqlClient client;
    auto connect_result = client.connect(cfg.host, cfg.port, cfg.user, cfg.password, cfg.database);
    if (!connect_result) {
        state->failed.fetch_add(static_cast<uint64_t>(cfg.queries_per_client), std::memory_order_relaxed);
        state->recordError("connect failed: " + connect_result.error().message());
        return;
    }

    for (size_t i = 0; i < cfg.warmup_queries; ++i) {
        auto _ = client.query(cfg.sql);
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
            auto begin_tx = client.beginTransaction();
            if (!begin_tx) {
                state->failed.fetch_add(static_cast<uint64_t>(batch_size), std::memory_order_relaxed);
                state->recordError("begin transaction failed: " + begin_tx.error().message());
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
            auto pipeline_result = client.batch(command_span);
            const auto finished = std::chrono::steady_clock::now();

            batch_elapsed = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(finished - started).count());

            if (cfg.alloc_stats) {
                const auto alloc_after = alloc_stats::snapshot();
                batch_alloc_count = alloc_after.alloc_count - alloc_before.alloc_count;
                batch_alloc_bytes = alloc_after.alloc_bytes - alloc_before.alloc_bytes;
            }

            if (!pipeline_result) {
                state->recordError("pipeline failed: " + pipeline_result.error().message());
            } else if (pipeline_result->size() != batch_size) {
                state->recordError("pipeline response count mismatch");
            } else {
                batch_success = batch_size;
            }
        } else {
            for (size_t j = 0; j < batch_size; ++j) {
                uint64_t elapsed_ns = 0;
                uint64_t alloc_count_delta = 0;
                uint64_t alloc_bytes_delta = 0;
                const bool ok = runSingleQuery(client, cfg, elapsed_ns, alloc_count_delta, alloc_bytes_delta);

                batch_elapsed += elapsed_ns;
                batch_alloc_count += alloc_count_delta;
                batch_alloc_bytes += alloc_bytes_delta;
                if (ok) {
                    ++batch_success;
                } else {
                    state->recordError("query failed in worker");
                }
            }
        }

        if (is_batch) {
            auto end_tx = client.commit();
            if (!end_tx) {
                state->recordError("commit failed: " + end_tx.error().message());
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

    state->alloc_count_delta.fetch_add(local_alloc_count, std::memory_order_relaxed);
    state->alloc_bytes_delta.fetch_add(local_alloc_bytes, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(state->latency_mutex);
        state->latencies_ns.insert(state->latencies_ns.end(), local_latencies.begin(), local_latencies.end());
    }
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

    std::cout << "\n=== B1 Sync Pressure Summary ===\n"
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
    std::cout << "Running sync pressure benchmark..." << std::endl;

    BenchmarkState state;
    {
        std::lock_guard<std::mutex> lock(state.latency_mutex);
        state.latencies_ns.reserve(cfg.clients * cfg.queries_per_client);
    }
    std::vector<std::thread> workers;
    workers.reserve(cfg.clients);

    const auto started = std::chrono::steady_clock::now();
    for (size_t i = 0; i < cfg.clients; ++i) {
        workers.emplace_back(runWorker, std::cref(cfg), &state);
    }

    for (auto& worker : workers) {
        worker.join();
    }
    const auto finished = std::chrono::steady_clock::now();

    printSummary(cfg, state, started, finished);
    return state.failed.load(std::memory_order_relaxed) == 0 ? 0 : 1;
}
