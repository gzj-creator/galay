/**
 * @file b35_parallel_work_item.cc
 * @brief Compare a graph of synchronous work items with one coroutine per item.
 *
 * This benchmark is a regression guard for the important design choice in
 * `parallel.h`: ordinary CPU work must not need a coroutine frame per node.
 */

#include "../common/benchmark_environment.h"

#include "benchmark/cpp/common/benchmark_sync.h"
#include <galay/cpp/galay-kernel/core/runtime.h>
#include <galay/cpp/galay-kernel/parallel/parallel.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <iomanip>
#include <iostream>

using namespace galay::kernel;

namespace {

constexpr std::size_t kNodes = 2000;

Task<void> graph_batch(std::size_t nodes,
                      galay::benchmark::CompletionLatch* completion,
                      std::atomic<std::size_t>* executed)
{
    ParallelGraph graph;
    for (std::size_t index = 0; index < nodes; ++index) {
        auto added = graph.add(make_parallel_work([executed]() noexcept {
            executed->fetch_add(1, std::memory_order_relaxed);
        }));
        if (!added.has_value()) {
            completion->arrive();
            co_return;
        }
    }

    auto result = co_await parallel(std::move(graph));
    if (result.has_value()) {
        completion->arrive();
    } else {
        completion->arrive();
    }
    co_return;
}

Task<void> coroutine_node(galay::benchmark::CompletionLatch* completion,
                         std::atomic<std::size_t>* executed)
{
    executed->fetch_add(1, std::memory_order_relaxed);
    completion->arrive();
    co_return;
}

Task<void> coroutine_batch(std::size_t nodes,
                          galay::benchmark::CompletionLatch* completion,
                          std::atomic<std::size_t>* executed)
{
    auto runtime = RuntimeHandle::current();
    if (!runtime.has_value()) {
        completion->arrive(nodes);
        co_return;
    }

    for (std::size_t index = 0; index < nodes; ++index) {
        auto submitted = runtime->spawn_cpu(coroutine_node(completion, executed));
        if (!submitted.has_value()) {
            completion->arrive();
        }
    }
    co_return;
}

template <typename Submit>
std::int64_t measure(Submit&& submit,
                     std::size_t target,
                     std::atomic<std::size_t>* executed)
{
    galay::benchmark::CompletionLatch completion(target);
    const auto started = std::chrono::steady_clock::now();
    submit(&completion, executed);
    completion.wait();
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now() - started)
        .count();
}

} // namespace

int main()
{
    if (!galay::benchmark::initialize_benchmark_environment()) {
        return 1;
    }

    Runtime runtime = RuntimeBuilder().io_scheduler_count(0).parallel_scheduler_count(4).build();
    const auto started = runtime.start();
    if (!started.has_value()) {
        std::cerr << "parallel_work_item runtime_start_failed\n";
        return 1;
    }

    std::atomic<std::size_t> graph_executed{0};
    const auto graph_us = measure(
        [&runtime](auto* completion, auto* executed) {
            auto submitted = runtime.spawn_cpu(graph_batch(kNodes, completion, executed));
            if (!submitted.has_value()) {
                completion->arrive();
            }
        },
        1,
        &graph_executed);

    std::atomic<std::size_t> coroutine_executed{0};
    const auto coroutine_us = measure(
        [&runtime](auto* completion, auto* executed) {
            auto submitted = runtime.spawn_cpu(coroutine_batch(kNodes, completion, executed));
            if (!submitted.has_value()) {
                completion->arrive(kNodes);
            }
        },
        kNodes,
        &coroutine_executed);

    runtime.stop();

    const bool counts_ok = graph_executed.load(std::memory_order_relaxed) == kNodes &&
                           coroutine_executed.load(std::memory_order_relaxed) == kNodes;
    std::cout << "parallel_work_item nodes=" << kNodes
              << " graph_us=" << graph_us
              << " coroutine_us=" << coroutine_us
              << " graph_tasks_s=" << std::fixed << std::setprecision(0)
              << (graph_us > 0 ? static_cast<double>(kNodes) * 1'000'000.0 / graph_us : 0.0)
              << " coroutine_tasks_s="
              << (coroutine_us > 0 ? static_cast<double>(kNodes) * 1'000'000.0 / coroutine_us : 0.0)
              << " status=" << (counts_ok ? "ok" : "failed") << '\n';
    return counts_ok && graph_us > 0 && coroutine_us > 0 ? 0 : 1;
}
