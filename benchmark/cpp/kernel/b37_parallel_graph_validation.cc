/**
 * @file b37_parallel_graph_validation.cc
 * @brief 测量反向链上的无分配并行图验证性能。
 */

#include <galay/cpp/galay-kernel/core/runtime.h>
#include <galay/cpp/galay-kernel/parallel/parallel.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <iostream>
#include <vector>

using namespace galay::kernel;

namespace {

Task<void> validateReverseChain(std::size_t node_count,
                                std::atomic<bool>* ok)
{
    ParallelGraph graph;
    std::vector<ParallelNodeId> nodes;
    nodes.reserve(node_count);
    for (std::size_t index = 0; index < node_count; ++index) {
        auto node = graph.add(makeParallelWork([]() noexcept {}));
        if (!node.has_value()) {
            ok->store(false, std::memory_order_release);
            co_return;
        }
        nodes.push_back(*node);
    }
    for (std::size_t index = 1; index < nodes.size(); ++index) {
        if (!graph.then(nodes[index], nodes[index - 1]).has_value()) {
            ok->store(false, std::memory_order_release);
            co_return;
        }
    }

    const auto result = co_await parallel(std::move(graph));
    ok->store(result.has_value(), std::memory_order_release);
    co_return;
}

} // namespace

int main()
{
    Runtime runtime = RuntimeBuilder()
        .ioSchedulerCount(0)
        // 让链始终在一个 worker 上执行，使测量反映图验证本身，而不是跨线程
        // 队列交接造成的额外开销。
        .parallelSchedulerCount(1)
        .build();
    if (!runtime.start().has_value()) {
        std::cerr << "parallel_graph_validation runtime_start_failed\n";
        return 1;
    }

    constexpr std::size_t kWarmupNodes = 2048;
    constexpr std::size_t kMeasuredNodes = 16384;
    std::atomic<bool> warmup_ok{false};
    (void)runtime.blockOnCpu(validateReverseChain(kWarmupNodes, &warmup_ok));

    std::atomic<bool> measured_ok{false};
    const auto begin = std::chrono::steady_clock::now();
    const auto result = runtime.blockOnCpu(
        validateReverseChain(kMeasuredNodes, &measured_ok));
    const auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - begin);
    runtime.stop();

    const bool ok = warmup_ok.load(std::memory_order_acquire) &&
        measured_ok.load(std::memory_order_acquire) && result.has_value();
    const double nodes_per_second = elapsed_us.count() > 0
        ? static_cast<double>(kMeasuredNodes) * 1'000'000.0 /
            static_cast<double>(elapsed_us.count())
        : 0.0;
    std::cout << "parallel_graph_validation nodes=" << kMeasuredNodes
              << " elapsed_us=" << elapsed_us.count()
              << " nodes_per_sec=" << nodes_per_second
              << " status=" << (ok ? "ok" : "failed") << '\n';
    return ok ? 0 : 1;
}
