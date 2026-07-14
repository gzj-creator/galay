#include "config.h"

#include <mysql.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace
{

struct BenchmarkState
{
    std::mutex samples_mutex;
    std::mutex error_mutex;
    std::vector<std::uint64_t> samples_ns;
    std::string first_error;
    std::atomic<std::uint64_t> success{0};
    std::atomic<std::uint64_t> failed{0};
};

void rememberError(BenchmarkState* state, std::string message)
{
    std::lock_guard lock(state->error_mutex);
    if (state->first_error.empty()) {
        state->first_error = std::move(message);
    }
}

bool runQuery(MYSQL* connection, const std::string& sql)
{
    if (mysql_real_query(connection, sql.data(), sql.size()) != 0) {
        return false;
    }

    MYSQL_RES* result = mysql_store_result(connection);
    if (result != nullptr) {
        mysql_free_result(result);
        return true;
    }
    return mysql_field_count(connection) == 0;
}

void runWorker(const mysql_benchmark::DbBenchmarkConfig* config, BenchmarkState* state)
{
    if (mysql_thread_init() != 0) {
        state->failed.fetch_add(config->queries_per_client, std::memory_order_relaxed);
        rememberError(state, "mysql_thread_init failed");
        return;
    }

    MYSQL* connection = mysql_init(nullptr);
    if (connection == nullptr) {
        state->failed.fetch_add(config->queries_per_client, std::memory_order_relaxed);
        rememberError(state, "mysql_init failed");
        mysql_thread_end();
        return;
    }

    const unsigned timeout_seconds = static_cast<unsigned>(
        std::min(config->timeout_seconds,
                 static_cast<std::size_t>(std::numeric_limits<unsigned>::max())));
    if (mysql_options(connection, MYSQL_OPT_CONNECT_TIMEOUT, &timeout_seconds) != 0 ||
        mysql_options(connection, MYSQL_OPT_READ_TIMEOUT, &timeout_seconds) != 0 ||
        mysql_options(connection, MYSQL_OPT_WRITE_TIMEOUT, &timeout_seconds) != 0) {
        state->failed.fetch_add(config->queries_per_client, std::memory_order_relaxed);
        rememberError(state, mysql_error(connection));
        mysql_close(connection);
        mysql_thread_end();
        return;
    }

    if (mysql_real_connect(connection,
                           config->host.c_str(),
                           config->user.c_str(),
                           config->password.c_str(),
                           config->database.c_str(),
                           config->port,
                           nullptr,
                           0) == nullptr) {
        state->failed.fetch_add(config->queries_per_client, std::memory_order_relaxed);
        rememberError(state, mysql_error(connection));
        mysql_close(connection);
        mysql_thread_end();
        return;
    }

    for (std::size_t warmup = 0; warmup < config->warmup_queries; ++warmup) {
        if (!runQuery(connection, config->sql)) {
            state->failed.fetch_add(config->queries_per_client, std::memory_order_relaxed);
            rememberError(state, mysql_error(connection));
            mysql_close(connection);
            mysql_thread_end();
            return;
        }
    }

    std::vector<std::uint64_t> local_samples;
    local_samples.reserve(config->queries_per_client);
    std::size_t completed = 0;
    while (completed < config->queries_per_client) {
        const bool transaction_batch = config->mode == mysql_benchmark::BenchmarkMode::Batch;
        const std::size_t batch_size = transaction_batch
            ? std::min(config->batch_size, config->queries_per_client - completed)
            : 1;

        bool transaction_ok = true;
        if (transaction_batch) {
            transaction_ok = runQuery(connection, "START TRANSACTION");
        }

        std::size_t batch_success = 0;
        for (std::size_t query = 0; query < batch_size; ++query) {
            const auto begin = std::chrono::steady_clock::now();
            const bool ok = transaction_ok && runQuery(connection, config->sql);
            const auto end = std::chrono::steady_clock::now();
            local_samples.push_back(static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin).count()));
            if (ok) {
                ++batch_success;
            }
            if (!ok) {
                rememberError(state, mysql_error(connection));
            }
        }

        bool commit_ok = transaction_ok;
        if (transaction_batch && transaction_ok) {
            commit_ok = runQuery(connection, "COMMIT");
            if (!commit_ok) {
                rememberError(state, mysql_error(connection));
            }
        } else if (!transaction_batch) {
            commit_ok = true;
        }
        const std::size_t committed = commit_ok ? batch_success : 0;
        state->success.fetch_add(committed, std::memory_order_relaxed);
        state->failed.fetch_add(batch_size - committed, std::memory_order_relaxed);
        completed += batch_size;
    }

    {
        std::lock_guard lock(state->samples_mutex);
        state->samples_ns.insert(
            state->samples_ns.end(), local_samples.begin(), local_samples.end());
    }
    mysql_close(connection);
    mysql_thread_end();
}

double percentile(const std::vector<std::uint64_t>& sorted_samples, double fraction)
{
    if (sorted_samples.empty()) {
        return 0.0;
    }
    const auto index = static_cast<std::size_t>(
        fraction * static_cast<double>(sorted_samples.size() - 1));
    return static_cast<double>(sorted_samples[index]) / 1e6;
}

} // namespace

int main(int argc, char* argv[])
{
    auto config = mysql_benchmark::loadDbBenchmarkConfig();
    if (!mysql_benchmark::parseArgs(config, argc, argv, std::cerr)) {
        mysql_benchmark::printUsage(argv[0]);
        return 2;
    }
    if (config.mode == mysql_benchmark::BenchmarkMode::Pipeline) {
        std::cerr << "libmysqlclient synchronous API has no equivalent Galay pipeline mode\n";
        return 2;
    }
    if (mysql_library_init(0, nullptr, nullptr) != 0) {
        std::cerr << "mysql_library_init failed\n";
        return 1;
    }

    BenchmarkState state;
    state.samples_ns.reserve(config.clients * config.queries_per_client);
    std::vector<std::thread> workers;
    workers.reserve(config.clients);

    const auto begin = std::chrono::steady_clock::now();
    for (std::size_t worker = 0; worker < config.clients; ++worker) {
        workers.emplace_back(runWorker, &config, &state);
    }
    for (auto& worker : workers) {
        worker.join();
    }
    const auto end = std::chrono::steady_clock::now();
    mysql_library_end();

    std::sort(state.samples_ns.begin(), state.samples_ns.end());
    const auto success = state.success.load(std::memory_order_relaxed);
    const auto failed = state.failed.load(std::memory_order_relaxed);
    const double seconds = std::chrono::duration<double>(end - begin).count();
    const double qps = seconds > 0.0 ? static_cast<double>(success) / seconds : 0.0;

    std::cout << "\n=== libmysqlclient Query Pressure Summary ===\n"
              << "mode: " << mysql_benchmark::modeToString(config.mode) << '\n'
              << "clients: " << config.clients << '\n'
              << "queries_per_client: " << config.queries_per_client << '\n'
              << "total_queries: " << success + failed << '\n'
              << "success: " << success << '\n'
              << "failed: " << failed << '\n'
              << "elapsed_sec: " << seconds << '\n'
              << "qps: " << qps << '\n'
              << "p50_latency_ms: " << percentile(state.samples_ns, 0.50) << '\n'
              << "p95_latency_ms: " << percentile(state.samples_ns, 0.95) << '\n'
              << "p99_latency_ms: " << percentile(state.samples_ns, 0.99) << '\n'
              << "max_latency_ms: " << percentile(state.samples_ns, 1.0) << '\n';
    if (!state.first_error.empty()) {
        std::cout << "first_error: " << state.first_error << '\n';
    }

    return failed == 0 ? 0 : 1;
}
