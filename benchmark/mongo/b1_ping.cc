#include "benchmark/common/bench_common.h"
#include "mongo/sync/mongo_client.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <new>
#include <thread>
#include <vector>

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

int main(int argc, char** argv)
{
    if (argc > 1 && (std::string(argv[1]) == "-h" || std::string(argv[1]) == "--help")) {
        mongo_bench::printUsage(argv[0]);
        return 0;
    }

    auto cfg = mongo_bench::loadBenchConfig();
    if (!mongo_bench::parseArgs(cfg, argc, argv, std::cerr)) {
        mongo_bench::printUsage(argv[0]);
        return 2;
    }
    mongo_bench::printBenchConfig("B1-SyncPingBench", cfg);

    if (cfg.mode != mongo_bench::BenchMode::Normal) {
        std::cerr << "B1-SyncPingBench currently supports only --mode normal" << std::endl;
        return 2;
    }

    std::vector<std::unique_ptr<MongoClient>> sessions;
    sessions.reserve(cfg.concurrency);

    const auto mongo_cfg = mongo_bench::toMongoConfig(cfg);
    for (size_t i = 0; i < cfg.concurrency; ++i) {
        auto session = std::make_unique<MongoClient>();
        auto conn = session->connect(mongo_cfg);
        if (!conn) {
            std::cerr << "connect failed (worker " << i << "): "
                      << conn.error().message() << std::endl;
            return 1;
        }
        sessions.push_back(std::move(session));
    }

    std::atomic<size_t> next{0};
    std::atomic<size_t> ok{0};
    std::atomic<size_t> error{0};
    std::atomic<uint64_t> alloc_count{0};
    std::atomic<uint64_t> alloc_bytes{0};

    std::mutex lat_mutex;
    std::vector<double> latencies_ms;
    latencies_ms.reserve(cfg.total_requests);

    const auto start = std::chrono::steady_clock::now();

    std::vector<std::thread> workers;
    workers.reserve(cfg.concurrency);

    for (size_t i = 0; i < cfg.concurrency; ++i) {
        workers.emplace_back([&, i]() {
            std::vector<double> local_lat;
            local_lat.reserve((cfg.total_requests / cfg.concurrency) + 8);
            size_t local_ok = 0;
            size_t local_error = 0;
            uint64_t local_alloc_count = 0;
            uint64_t local_alloc_bytes = 0;

            MongoClient& session = *sessions[i];
            while (true) {
                const size_t index = next.fetch_add(1, std::memory_order_relaxed);
                if (index >= cfg.total_requests) {
                    break;
                }

                const auto alloc_before = cfg.alloc_stats
                    ? alloc_stats::snapshot()
                    : alloc_stats::Snapshot{};
                const auto t0 = std::chrono::steady_clock::now();
                const auto result = session.ping(cfg.database);
                const auto t1 = std::chrono::steady_clock::now();
                const auto alloc_after = cfg.alloc_stats
                    ? alloc_stats::snapshot()
                    : alloc_stats::Snapshot{};

                const double latency =
                    std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(t1 - t0)
                        .count();
                local_lat.push_back(latency);

                if (result) {
                    ++local_ok;
                } else {
                    ++local_error;
                }

                if (cfg.alloc_stats) {
                    local_alloc_count += alloc_after.alloc_count - alloc_before.alloc_count;
                    local_alloc_bytes += alloc_after.alloc_bytes - alloc_before.alloc_bytes;
                }
            }

            ok.fetch_add(local_ok, std::memory_order_relaxed);
            error.fetch_add(local_error, std::memory_order_relaxed);
            alloc_count.fetch_add(local_alloc_count, std::memory_order_relaxed);
            alloc_bytes.fetch_add(local_alloc_bytes, std::memory_order_relaxed);

            std::lock_guard<std::mutex> lock(lat_mutex);
            latencies_ms.insert(latencies_ms.end(), local_lat.begin(), local_lat.end());
        });
    }

    for (auto& worker : workers) {
        worker.join();
    }

    const auto end = std::chrono::steady_clock::now();
    const auto duration_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    for (auto& session : sessions) {
        session->close();
    }

    const size_t ok_count = ok.load(std::memory_order_relaxed);
    const size_t err_count = error.load(std::memory_order_relaxed);

    mongo_bench::printBenchReport(cfg.total_requests,
                                  ok_count,
                                  err_count,
                                  duration_ms,
                                  latencies_ms,
                                  cfg.alloc_stats,
                                  alloc_count.load(std::memory_order_relaxed),
                                  alloc_bytes.load(std::memory_order_relaxed));

    return err_count == 0 ? 0 : 1;
}
