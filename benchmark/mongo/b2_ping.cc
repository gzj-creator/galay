#include "common/bench_common.h"
#include "galay-mongo/async/client.h"
#include "galay-mongo/protoc/builder.h"

#include <galay-kernel/core/runtime.h>

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

using namespace galay::kernel;
using namespace galay::mongo;

namespace
{
namespace alloc_stats
{
std::atomic<uint64_t> g_alloc_count{0};
std::atomic<uint64_t> g_alloc_bytes{0};

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

size_t parsePositiveSize(const char* text, size_t fallback)
{
    if (text == nullptr || text[0] == '\0') {
        return fallback;
    }
    try {
        const unsigned long long parsed = std::stoull(text);
        return parsed == 0ULL ? fallback : static_cast<size_t>(parsed);
    } catch (...) {
        return fallback;
    }
}

size_t loadAsyncFanout(int argc, char** argv)
{
    size_t fanout = parsePositiveSize(std::getenv("GALAY_MONGO_BENCH_ASYNC_FANOUT"), 1);
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        constexpr std::string_view prefix = "--fanout=";
        if (arg.rfind(prefix, 0) == 0) {
            const std::string value(arg.substr(prefix.size()));
            fanout = parsePositiveSize(value.c_str(), fanout);
        }
    }
    if (argc > 9 && argv[9] != nullptr && argv[9][0] != '\0') {
        fanout = parsePositiveSize(argv[9], fanout);
    }
    return fanout;
}

bool isFanoutArg(const char* arg)
{
    if (arg == nullptr) {
        return false;
    }
    return std::string_view(arg).rfind("--fanout=", 0) == 0;
}

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

struct AsyncBenchState
{
    std::atomic<size_t> next{0};
    std::atomic<size_t> attempted{0};
    std::atomic<size_t> ok{0};
    std::atomic<size_t> error{0};
    std::atomic<uint64_t> alloc_count{0};
    std::atomic<uint64_t> alloc_bytes{0};
    std::atomic<size_t> done_workers{0};

    std::mutex latency_mutex;
    std::vector<double> latencies_ms;

    std::mutex error_mutex;
    std::string first_error;

    void setFirstError(std::string message)
    {
        std::lock_guard<std::mutex> lock(error_mutex);
        if (first_error.empty()) {
            first_error = std::move(message);
        }
    }
};

Task<void> runWorker(IOScheduler* scheduler,
                     AsyncBenchState* state,
                     mongo_bench::BenchConfig cfg,
                     size_t worker_count)
{
    auto client = AsyncMongoClientBuilder()
        .scheduler(scheduler)
        .bufferSize(cfg.buffer_size)
        .build();

    std::vector<double> local_lat;
    local_lat.reserve((cfg.total_requests / worker_count) + 8);

    const std::expected<bool, MongoError> conn_result =
        mongo_bench::unwrapMongoTaskResult(
            co_await client.connect(mongo_bench::toMongoConfig(cfg)),
            MONGO_ERROR_CONNECTION);
    if (!conn_result) {
        state->error.fetch_add(1, std::memory_order_relaxed);
        state->setFirstError("connect failed: " + conn_result.error().message());
        state->done_workers.fetch_add(1, std::memory_order_release);
        co_return;
    }

    protocol::MongoCommandBuilder full_pipeline_builder;
    protocol::MongoCommandBuilder tail_pipeline_builder;
    std::span<const MongoDocument> full_pipeline_commands;
    std::span<const MongoDocument> tail_pipeline_commands;
    size_t tail_batch_size = 0;

    if (cfg.mode == mongo_bench::BenchMode::Pipeline) {
        full_pipeline_builder.reserve(cfg.batch_size);
        for (size_t i = 0; i < cfg.batch_size; ++i) {
            full_pipeline_builder.appendPing();
        }
        full_pipeline_commands = full_pipeline_builder.commands();

        tail_batch_size = cfg.total_requests % cfg.batch_size;
        if (tail_batch_size > 0) {
            tail_pipeline_builder.reserve(tail_batch_size);
            for (size_t i = 0; i < tail_batch_size; ++i) {
                tail_pipeline_builder.appendPing();
            }
            tail_pipeline_commands = tail_pipeline_builder.commands();
        }
    }

    size_t local_attempted = 0;
    size_t local_ok = 0;
    size_t local_error = 0;
    uint64_t local_alloc_count = 0;
    uint64_t local_alloc_bytes = 0;

    while (true) {
        const size_t chunk = cfg.mode == mongo_bench::BenchMode::Pipeline ? cfg.batch_size : 1;
        const size_t start_index = state->next.fetch_add(chunk, std::memory_order_relaxed);
        if (start_index >= cfg.total_requests) {
            break;
        }
        const size_t remaining = cfg.total_requests - start_index;
        const size_t actual_batch = std::min(chunk, remaining);
        local_attempted += actual_batch;

        if (cfg.mode == mongo_bench::BenchMode::Normal) {
            const auto alloc_before = cfg.alloc_stats
                ? alloc_stats::snapshot()
                : alloc_stats::Snapshot{};
            const auto t0 = std::chrono::steady_clock::now();
            const std::expected<MongoReply, MongoError> cmd_result =
                mongo_bench::unwrapMongoTaskResult(co_await client.ping(cfg.database),
                                                   MONGO_ERROR_COMMAND);
            const auto t1 = std::chrono::steady_clock::now();
            const auto alloc_after = cfg.alloc_stats
                ? alloc_stats::snapshot()
                : alloc_stats::Snapshot{};

            const double latency =
                std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(t1 - t0).count();
            local_lat.push_back(latency);

            if (!cmd_result) {
                ++local_error;
                state->setFirstError("command failed: " + cmd_result.error().message());
            } else {
                ++local_ok;
            }

            if (cfg.alloc_stats) {
                local_alloc_count += alloc_after.alloc_count - alloc_before.alloc_count;
                local_alloc_bytes += alloc_after.alloc_bytes - alloc_before.alloc_bytes;
            }
            continue;
        }

        std::span<const MongoDocument> pipeline_commands;
        protocol::MongoCommandBuilder dynamic_pipeline_builder;
        if (actual_batch == cfg.batch_size) {
            pipeline_commands = full_pipeline_commands;
        } else if (actual_batch == tail_batch_size && tail_batch_size > 0) {
            pipeline_commands = tail_pipeline_commands;
        } else {
            dynamic_pipeline_builder.reserve(actual_batch);
            for (size_t i = 0; i < actual_batch; ++i) {
                dynamic_pipeline_builder.appendPing();
            }
            pipeline_commands = dynamic_pipeline_builder.commands();
        }

        const auto alloc_before = cfg.alloc_stats
            ? alloc_stats::snapshot()
            : alloc_stats::Snapshot{};
        const auto t0 = std::chrono::steady_clock::now();
        const std::expected<std::vector<MongoPipelineResponse>, MongoError> pipe_result =
            mongo_bench::unwrapMongoTaskResult(
                co_await client.pipeline(cfg.database, pipeline_commands),
                MONGO_ERROR_COMMAND);
        const auto t1 = std::chrono::steady_clock::now();
        const auto alloc_after = cfg.alloc_stats
            ? alloc_stats::snapshot()
            : alloc_stats::Snapshot{};

        const double elapsed_ms =
            std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(t1 - t0).count();
        const double per_op_latency = elapsed_ms / static_cast<double>(actual_batch);
        for (size_t i = 0; i < actual_batch; ++i) {
            local_lat.push_back(per_op_latency);
        }

        if (!pipe_result) {
            local_error += actual_batch;
            state->setFirstError("pipeline failed: " + pipe_result.error().message());
        } else if (pipe_result->size() != actual_batch) {
            local_error += actual_batch;
            state->setFirstError("pipeline response size mismatch");
        } else {
            for (const auto& item : *pipe_result) {
                if (item.ok()) {
                    ++local_ok;
                } else {
                    ++local_error;
                    if (item.error.has_value()) {
                        state->setFirstError("pipeline item failed: " + item.error->message());
                    }
                }
            }
        }

        if (cfg.alloc_stats) {
            local_alloc_count += alloc_after.alloc_count - alloc_before.alloc_count;
            local_alloc_bytes += alloc_after.alloc_bytes - alloc_before.alloc_bytes;
        }
    }

    state->attempted.fetch_add(local_attempted, std::memory_order_relaxed);
    state->ok.fetch_add(local_ok, std::memory_order_relaxed);
    state->error.fetch_add(local_error, std::memory_order_relaxed);
    state->alloc_count.fetch_add(local_alloc_count, std::memory_order_relaxed);
    state->alloc_bytes.fetch_add(local_alloc_bytes, std::memory_order_relaxed);

    co_await client.close();

    {
        std::lock_guard<std::mutex> lock(state->latency_mutex);
        state->latencies_ms.insert(state->latencies_ms.end(), local_lat.begin(), local_lat.end());
    }

    state->done_workers.fetch_add(1, std::memory_order_release);
}

int main(int argc, char** argv)
{
    if (argc > 1 && (std::string(argv[1]) == "-h" || std::string(argv[1]) == "--help")) {
        mongo_bench::printUsage(argv[0]);
        std::cout << "Async extra: --fanout=N or env GALAY_MONGO_BENCH_ASYNC_FANOUT (default 1)\n";
        return 0;
    }

    std::vector<char*> filtered_argv;
    filtered_argv.reserve(static_cast<size_t>(argc));
    filtered_argv.push_back(argv[0]);
    for (int i = 1; i < argc; ++i) {
        if (isFanoutArg(argv[i])) {
            continue;
        }
        filtered_argv.push_back(argv[i]);
    }

    auto cfg = mongo_bench::loadBenchConfig();
    if (!mongo_bench::parseArgs(
            cfg,
            static_cast<int>(filtered_argv.size()),
            filtered_argv.data(),
            std::cerr)) {
        mongo_bench::printUsage(argv[0]);
        return 2;
    }
    if (cfg.batch_size == 0) {
        cfg.batch_size = 1;
    }

    mongo_bench::printBenchConfig("B2-AsyncPingBench", cfg);
    const size_t async_fanout = loadAsyncFanout(argc, argv);
    const size_t worker_count = cfg.concurrency * async_fanout;
    std::cout << "[B2-AsyncPingBench]"
              << " async_fanout=" << async_fanout
              << " worker_count=" << worker_count
              << std::endl;

    Runtime runtime;
    runtime.start();

    AsyncBenchState state;
    state.latencies_ms.reserve(cfg.total_requests);

    const auto start = std::chrono::steady_clock::now();

    for (size_t i = 0; i < worker_count; ++i) {
        IOScheduler* scheduler = runtime.getNextIOScheduler();
        if (!scheduler) {
            runtime.stop();
            std::cerr << "failed to get IO scheduler" << std::endl;
            return 1;
        }
        if (!scheduleTask(scheduler, runWorker(scheduler, &state, cfg, worker_count))) {
            runtime.stop();
            std::cerr << "failed to schedule benchmark worker" << std::endl;
            return 1;
        }
    }

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(cfg.timeout_seconds);
    while (state.done_workers.load(std::memory_order_acquire) < worker_count &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    const bool timeout = state.done_workers.load(std::memory_order_acquire) < worker_count;
    const auto end = std::chrono::steady_clock::now();
    const auto duration_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    runtime.stop();

    const size_t ok_count = state.ok.load(std::memory_order_relaxed);
    const size_t err_count = state.error.load(std::memory_order_relaxed);
    const size_t attempted = state.attempted.load(std::memory_order_relaxed);

    mongo_bench::printBenchReport(attempted,
                                  ok_count,
                                  err_count,
                                  duration_ms,
                                  state.latencies_ms,
                                  cfg.alloc_stats,
                                  state.alloc_count.load(std::memory_order_relaxed),
                                  state.alloc_bytes.load(std::memory_order_relaxed));

    if (!state.first_error.empty()) {
        std::cout << "First error: " << state.first_error << std::endl;
    }

    if (timeout) {
        std::cerr << "benchmark timeout" << std::endl;
        return 2;
    }

    return err_count == 0 ? 0 : 1;
}
