// Historical/internal-only fixture. Not a formal competitor baseline; see docs/cpp/modules/kernel/05-性能测试.md.
#include "config.h"

#include <libpq-fe.h>

#include <algorithm>
#include <atomic>
#include <barrier>
#include <chrono>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <vector>

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

PGconn* connect(const postgres_benchmark::Config& config)
{
    const std::string port = std::to_string(config.port);
    const char* keywords[] = {
        "host", "port", "user", "password", "dbname", "connect_timeout",
        "application_name", nullptr};
    const char* values[] = {
        config.host.c_str(), port.c_str(), config.user.c_str(), config.password.c_str(),
        config.database.c_str(), "5", "galay-libpq-prepared-benchmark", nullptr};
    PGconn* connection = PQconnectdbParams(keywords, values, 0);
    if (connection == nullptr || PQstatus(connection) != CONNECTION_OK) {
        return connection;
    }

    const int socket_fd = PQsocket(connection);
    const int enabled = 1;
    if (socket_fd < 0 ||
        ::setsockopt(socket_fd, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled)) != 0) {
        PQfinish(connection);
        return nullptr;
    }
    return connection;
}

bool executePrepared(PGconn* connection, size_t value, uint64_t* result_value)
{
    const std::string parameter = std::to_string(value);
    const char* parameters[] = {parameter.c_str()};
    PGresult* result = PQexecPrepared(connection,
                                       "galay_benchmark",
                                       1,
                                       parameters,
                                       nullptr,
                                       nullptr,
                                       0);
    if (result == nullptr) {
        return false;
    }
    const bool succeeded = PQresultStatus(result) == PGRES_TUPLES_OK &&
        PQntuples(result) == 1 && PQnfields(result) == 1;
    uint64_t parsed = 0;
    if (succeeded) {
        const char* text = PQgetvalue(result, 0, 0);
        const char* end = text + PQgetlength(result, 0, 0);
        const auto [parsed_end, error] = std::from_chars(text, end, parsed);
        if (error != std::errc{} || parsed_end != end) {
            PQclear(result);
            return false;
        }
    }
    PQclear(result);
    if (!succeeded) {
        return false;
    }
    *result_value = parsed;
    return true;
}

void runWorker(const postgres_benchmark::Config* config,
               BenchmarkState* state,
               std::barrier<>* ready_barrier,
               std::barrier<>* start_barrier,
               std::barrier<>* finish_barrier)
{
    PGconn* connection = connect(*config);
    bool ready = connection != nullptr && PQstatus(connection) == CONNECTION_OK;
    if (ready) {
        PGresult* prepared = PQprepare(connection,
                                       "galay_benchmark",
                                       "SELECT $1::bigint",
                                       1,
                                       nullptr);
        ready = prepared != nullptr && PQresultStatus(prepared) == PGRES_COMMAND_OK;
        if (prepared != nullptr) {
            PQclear(prepared);
        }
    }
    if (ready) {
        for (size_t index = 0; index < kWarmupQueries; ++index) {
            uint64_t result_value = 0;
            if (!executePrepared(connection, index, &result_value)) {
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
        if (connection != nullptr) {
            PQfinish(connection);
        }
        return;
    }

    uint64_t local_succeeded = 0;
    uint64_t local_failed = 0;
    uint64_t local_checksum = 0;
    std::vector<uint64_t> local_samples;
    local_samples.reserve(config->queries);
    for (size_t iteration = 0; iteration < config->queries; ++iteration) {
        uint64_t result_value = 0;
        const auto started = std::chrono::steady_clock::now();
        if (!executePrepared(connection, iteration, &result_value)) {
            const auto finished = std::chrono::steady_clock::now();
            local_samples.push_back(static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(finished - started).count()));
            ++local_failed;
            continue;
        }
        const auto finished = std::chrono::steady_clock::now();
        local_samples.push_back(static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(finished - started).count()));
        ++local_succeeded;
        local_checksum += result_value;
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
    PQfinish(connection);
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
