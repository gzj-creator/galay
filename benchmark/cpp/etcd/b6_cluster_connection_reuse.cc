/**
 * @file b6_cluster_connection_reuse.cc
 * @brief 测量同步与异步 cluster client 无锁租约池的 acquire/release 吞吐。
 */

#include <galay/cpp/galay-etcd/async/client.h>
#include <galay/cpp/galay-etcd/cluster/etcd_cluster_client.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace
{

template <typename Pool>
std::pair<int64_t, size_t> runConcurrentAcquire(
    Pool& pool,
    size_t total_iterations,
    size_t thread_count)
{
    std::atomic<size_t> failures{0};
    std::vector<std::thread> workers;
    workers.reserve(thread_count);

    const auto begin = std::chrono::steady_clock::now();
    for (size_t thread_index = 0; thread_index < thread_count; ++thread_index) {
        const size_t iterations = total_iterations / thread_count +
            (thread_index < total_iterations % thread_count ? 1 : 0);
        workers.emplace_back([&pool, &failures, iterations] {
            for (size_t iteration = 0; iteration < iterations; ++iteration) {
                auto lease = pool.tryAcquire();
                if (!lease.has_value()) {
                    failures.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }
    const auto end = std::chrono::steady_clock::now();
    return {
        std::chrono::duration_cast<std::chrono::microseconds>(end - begin).count(),
        failures.load(std::memory_order_relaxed),
    };
}

} // namespace

int main(int argc, char** argv)
{
    const long parsed_iterations = argc > 1 ? std::strtol(argv[1], nullptr, 10) : 1'000'000;
    const size_t iterations = parsed_iterations > 0
        ? static_cast<size_t>(parsed_iterations)
        : size_t{1'000'000};

    galay::etcd::EtcdProductionConfig production;
    production.endpoints = {
        "http://127.0.0.1:2379",
        "http://127.0.0.1:22379",
        "http://127.0.0.1:32379",
    };
    production.connections_per_endpoint = 4;

    auto pool = galay::etcd::EtcdClusterClientBuilder()
        .productionConfig(production)
        .build();
    auto warmup = pool.tryAcquire();
    if (!warmup.has_value()) {
        std::cerr << "warmup acquire failed: " << warmup.error().message() << '\n';
        return 1;
    }
    warmup->release();

    const auto begin = std::chrono::steady_clock::now();
    for (size_t iteration = 0; iteration < iterations; ++iteration) {
        auto lease = pool.tryAcquire();
        if (!lease.has_value()) {
            std::cerr << "acquire failed at iteration " << iteration
                      << ": " << lease.error().message() << '\n';
            return 1;
        }
    }
    const auto end = std::chrono::steady_clock::now();

    production.connections_per_endpoint = 4;
    auto async_pool = galay::etcd::AsyncEtcdClusterClientBuilder()
        .scheduler(nullptr)
        .productionConfig(std::move(production))
        .build();
    const auto async_begin = std::chrono::steady_clock::now();
    for (size_t iteration = 0; iteration < iterations; ++iteration) {
        auto lease = async_pool.tryAcquire();
        if (!lease.has_value()) {
            std::cerr << "async acquire failed at iteration " << iteration
                      << ": " << lease.error().message() << '\n';
            return 1;
        }
    }
    const auto async_end = std::chrono::steady_clock::now();

    const auto elapsed_us =
        std::chrono::duration_cast<std::chrono::microseconds>(end - begin).count();
    const double operations_per_second = elapsed_us > 0
        ? static_cast<double>(iterations) * 1'000'000.0 / static_cast<double>(elapsed_us)
        : 0.0;
    const auto async_elapsed_us =
        std::chrono::duration_cast<std::chrono::microseconds>(async_end - async_begin).count();
    const double async_operations_per_second = async_elapsed_us > 0
        ? static_cast<double>(iterations) * 1'000'000.0 /
            static_cast<double>(async_elapsed_us)
        : 0.0;

    constexpr size_t kThreadCount = 8;
    const auto sync_concurrent = runConcurrentAcquire(pool, iterations, kThreadCount);
    if (sync_concurrent.second != 0) {
        std::cerr << "sync concurrent acquire failures: " << sync_concurrent.second << '\n';
        return 1;
    }
    const auto async_concurrent = runConcurrentAcquire(async_pool, iterations, kThreadCount);
    if (async_concurrent.second != 0) {
        std::cerr << "async concurrent acquire failures: " << async_concurrent.second << '\n';
        return 1;
    }
    const double sync_concurrent_ops = sync_concurrent.first > 0
        ? static_cast<double>(iterations) * 1'000'000.0 /
            static_cast<double>(sync_concurrent.first)
        : 0.0;
    const double async_concurrent_ops = async_concurrent.first > 0
        ? static_cast<double>(iterations) * 1'000'000.0 /
            static_cast<double>(async_concurrent.first)
        : 0.0;

    std::cout << "Pool size  : " << pool.size() << '\n';
    std::cout << "Iterations : " << iterations << '\n';
    std::cout << "Elapsed us : " << elapsed_us << '\n';
    std::cout << "Acquire/s  : " << operations_per_second << '\n';
    std::cout << "Async pool size : " << async_pool.size() << '\n';
    std::cout << "Async elapsed us: " << async_elapsed_us << '\n';
    std::cout << "Async acquire/s : " << async_operations_per_second << '\n';
    std::cout << "Threads          : " << kThreadCount << '\n';
    std::cout << "Sync shared acquire/s : " << sync_concurrent_ops << '\n';
    std::cout << "Async shared acquire/s: " << async_concurrent_ops << '\n';
    return 0;
}
