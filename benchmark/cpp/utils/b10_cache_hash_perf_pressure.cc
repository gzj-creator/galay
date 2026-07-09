/**
 * @file b10_cache_hash_perf_pressure.cc
 * @brief LRU 热 key 与一致性哈希 lookup 压力基准。
 *
 * 使用方法:
 *   ./benchmark_utils_cache_hash_perf_pressure [iterations] [threads]
 */

#include <galay/cpp/galay-utils/algorithm/consistent_hash.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#define private public
#include <galay/cpp/galay-utils/cache/lru_cache.hpp>
#undef private

namespace {

struct ManualClock {
    using rep = int64_t;
    using period = std::milli;
    using duration = std::chrono::duration<rep, period>;
    using time_point = std::chrono::time_point<ManualClock>;

    static constexpr bool is_steady = true;
    static inline time_point current{duration{0}};

    static time_point now() noexcept
    {
        return current;
    }

    static void reset()
    {
        current = time_point{duration{0}};
    }

    static void advance(duration delta)
    {
        current += delta;
    }
};

bool require(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "[benchmark_utils_cache_hash_perf_pressure] " << message << "\n";
        return false;
    }
    return true;
}

bool runLruHotKey(size_t iterations)
{
    using Cache = galay::utils::LruCache<std::string,
                                         int,
                                         std::hash<std::string>,
                                         std::equal_to<std::string>,
                                         ManualClock>;

    ManualClock::reset();
    Cache cache(4, ManualClock::duration{1000}, nullptr,
                Cache::ExpirationPolicy::ExpireAfterAccess);
    const bool inserted = cache.put("alpha", 1);
    if (!require(inserted, "LRU fixture insert failed")) {
        return false;
    }

    size_t checksum = 0;
    const auto start = std::chrono::steady_clock::now();
    for (size_t i = 0; i < iterations; ++i) {
        ManualClock::advance(ManualClock::duration{1});
        int* value = cache.get("alpha");
        if (value == nullptr) {
            std::cerr << "LRU hot key unexpectedly expired at iteration " << i << "\n";
            return false;
        }
        checksum += static_cast<size_t>(*value);
    }
    const auto end = std::chrono::steady_clock::now();
    const double seconds = std::chrono::duration<double>(end - start).count();
    if (!require(checksum == iterations, "LRU checksum mismatch")) {
        return false;
    }

    std::cout << "BM_LruHotKeyExpireAfterAccess: " << iterations
              << " gets, " << (static_cast<double>(iterations) / seconds)
              << " ops/s, expiration_heap=" << cache.m_expirations.size()
              << "\n";
    return cache.m_expirations.size() <= (iterations / 250) + 4;
}

bool runConsistentHashLookup(size_t iterations, size_t thread_count)
{
    galay::utils::ConsistentHash hash(128);
    hash.addNode({"node-a", "127.0.0.1:9001", 1});
    hash.addNode({"node-b", "127.0.0.1:9002", 1});
    hash.addNode({"node-c", "127.0.0.1:9003", 1});

    const std::array<std::string, 8> keys = {
        "order-1", "order-2", "order-3", "order-4",
        "order-5", "order-6", "order-7", "order-8",
    };

    std::vector<size_t> checksums(thread_count, 0);
    std::vector<std::thread> workers;
    workers.reserve(thread_count);

    const auto start = std::chrono::steady_clock::now();
    for (size_t t = 0; t < thread_count; ++t) {
        std::thread& worker = workers.emplace_back([&, t]() {
            size_t local = 0;
            for (size_t i = 0; i < iterations; ++i) {
                const std::string& key = keys[(i + t) % keys.size()];
                auto node = hash.getNode(key);
                if (node.has_value()) {
                    local += node->id.size();
                }
            }
            checksums[t] = local;
        });
        if (!worker.joinable()) {
            std::cerr << "worker thread is not joinable\n";
            return false;
        }
    }

    for (auto& worker : workers) {
        worker.join();
    }
    const auto end = std::chrono::steady_clock::now();

    size_t checksum = 0;
    for (size_t value : checksums) {
        checksum += value;
    }
    if (!require(checksum != 0, "consistent hash checksum should not be zero")) {
        return false;
    }

    const size_t total_ops = iterations * thread_count;
    const double seconds = std::chrono::duration<double>(end - start).count();
    std::cout << "BM_ConsistentHashLookupMultiThread: " << total_ops
              << " lookups, " << (static_cast<double>(total_ops) / seconds)
              << " ops/s, threads=" << thread_count
              << ", checksum=" << checksum << "\n";
    return true;
}

} // namespace

int main(int argc, char** argv)
{
    size_t iterations = 200000;
    size_t thread_count = 4;
    if (argc > 1) {
        const long requested = std::strtol(argv[1], nullptr, 10);
        if (requested > 0) {
            iterations = static_cast<size_t>(requested);
        }
    }
    if (argc > 2) {
        const long requested = std::strtol(argv[2], nullptr, 10);
        if (requested > 0) {
            thread_count = static_cast<size_t>(requested);
        }
    }

    if (!runLruHotKey(iterations)) {
        return 1;
    }
    if (!runConsistentHashLookup(iterations / 4 + 1, thread_count)) {
        return 1;
    }
    return 0;
}
