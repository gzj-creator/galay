/**
 * @file t181_parallel_dag.cc
 * @brief Boundary coverage for the parallel DAG and its completion wake-up.
 */

#include <galay/cpp/galay-kernel/parallel/parallel.h>

#include <atomic>
#include <cassert>
#include <cstddef>
#include <expected>
#include <iostream>

using namespace galay::kernel;

namespace {

Task<void> runDiamond(std::atomic<int>* completed,
                      std::atomic<bool>* parent_resumed,
                      std::atomic<bool>* ok)
{
    ParallelGraph graph;
    auto first = graph.add(makeParallelWork([completed]() noexcept {
        completed->fetch_or(1, std::memory_order_release);
    }));
    auto left = graph.add(makeParallelWork([completed]() noexcept {
        assert((completed->load(std::memory_order_acquire) & 1) != 0);
        completed->fetch_or(2, std::memory_order_release);
    }));
    auto right = graph.add(makeParallelWork([completed]() noexcept {
        assert((completed->load(std::memory_order_acquire) & 1) != 0);
        completed->fetch_or(4, std::memory_order_release);
    }));
    auto last = graph.add(makeParallelWork([completed]() noexcept {
        const auto value = completed->load(std::memory_order_acquire);
        assert((value & 6) == 6);
        completed->fetch_or(8, std::memory_order_release);
    }));
    assert(first.has_value() && left.has_value() && right.has_value() && last.has_value());
    assert(graph.then(*first, *left).has_value());
    assert(graph.then(*first, *right).has_value());
    assert(graph.then(*left, *last).has_value());
    assert(graph.then(*right, *last).has_value());

    const auto result = co_await parallel(std::move(graph));
    ok->store(result.has_value(), std::memory_order_release);
    parent_resumed->store(true, std::memory_order_release);
    co_return;
}

Task<void> runFailure(std::atomic<int>* completed,
                      std::atomic<bool>* ok,
                      std::atomic<bool>* skipped)
{
    ParallelGraph graph;
    auto first = graph.add(makeParallelWork([completed]() noexcept {
        completed->fetch_or(1, std::memory_order_release);
    }));
    auto failing = graph.add(makeParallelWork([]() noexcept -> std::expected<void, ParallelError> {
        return std::unexpected(ParallelError(ParallelErrorCode::kWorkFailed));
    }));
    auto independent = graph.add(makeParallelWork([completed]() noexcept {
        for (std::size_t i = 0; i < 10000; ++i) {
            completed->fetch_or(4, std::memory_order_relaxed);
        }
    }));
    auto dependent = graph.add(makeParallelWork([skipped]() noexcept {
        skipped->store(false, std::memory_order_release);
    }));
    assert(first.has_value() && failing.has_value() && independent.has_value() && dependent.has_value());
    assert(graph.then(*first, *failing).has_value());
    assert(graph.then(*failing, *dependent).has_value());

    const auto result = co_await parallel(std::move(graph));
    ok->store(!result.has_value() && result.error().code() == ParallelErrorCode::kWorkFailed,
              std::memory_order_release);
    co_return;
}

Task<void> runCycle(std::atomic<bool>* detected)
{
    ParallelGraph graph;
    auto first = graph.add(makeParallelWork([]() noexcept {}));
    auto second = graph.add(makeParallelWork([]() noexcept {}));
    assert(first.has_value() && second.has_value());
    assert(graph.then(*first, *second).has_value());
    assert(graph.then(*second, *first).has_value());

    const auto result = co_await parallel(std::move(graph));
    detected->store(!result.has_value() &&
                        result.error().code() == ParallelErrorCode::kCycleDetected,
                    std::memory_order_release);
    co_return;
}

Task<void> runSingle(std::atomic<int>* count, std::atomic<bool>* ok)
{
    ParallelGraph graph;
    auto node = graph.add(makeParallelWork([count]() noexcept {
        count->fetch_add(1, std::memory_order_release);
    }));
    assert(node.has_value());
    const auto result = co_await parallel(std::move(graph));
    ok->store(result.has_value(), std::memory_order_release);
    co_return;
}

Task<void> runEmpty(std::atomic<bool>* ok)
{
    const auto result = co_await parallel(ParallelGraph{});
    ok->store(result.has_value(), std::memory_order_release);
    co_return;
}

Task<void> runWithoutParallelScheduler(std::atomic<bool>* rejected)
{
    ParallelGraph graph;
    auto node = graph.add(makeParallelWork([]() noexcept {}));
    assert(node.has_value());
    const auto result = co_await parallel(std::move(graph));
    rejected->store(!result.has_value() &&
                        result.error().code() == ParallelErrorCode::kSchedulerUnavailable,
                    std::memory_order_release);
    co_return;
}

} // namespace

int main()
{
    Runtime runtime = RuntimeBuilder().ioSchedulerCount(0).parallelSchedulerCount(2).build();

    std::atomic<int> diamond_completed{0};
    std::atomic<bool> diamond_parent{false};
    std::atomic<bool> diamond_ok{false};
    auto diamond = runtime.blockOnCpu(runDiamond(&diamond_completed, &diamond_parent, &diamond_ok));
    assert(diamond.has_value());
    assert(diamond_ok.load(std::memory_order_acquire));
    assert(diamond_parent.load(std::memory_order_acquire));
    assert(diamond_completed.load(std::memory_order_acquire) == 15);

    std::atomic<int> failure_completed{0};
    std::atomic<bool> failure_ok{false};
    std::atomic<bool> dependent_ran{true};
    auto failure = runtime.blockOnCpu(runFailure(&failure_completed, &failure_ok, &dependent_ran));
    assert(failure.has_value());
    assert(failure_ok.load(std::memory_order_acquire));
    assert(dependent_ran.load(std::memory_order_acquire));
    assert((failure_completed.load(std::memory_order_acquire) & 4) != 0);

    std::atomic<bool> cycle_detected{false};
    auto cycle = runtime.blockOnCpu(runCycle(&cycle_detected));
    assert(cycle.has_value());
    assert(cycle_detected.load(std::memory_order_acquire));

    std::atomic<int> single_count{0};
    std::atomic<bool> single_ok{false};
    auto single = runtime.blockOnCpu(runSingle(&single_count, &single_ok));
    assert(single.has_value());
    assert(single_ok.load(std::memory_order_acquire));
    assert(single_count.load(std::memory_order_acquire) == 1);

    std::atomic<bool> empty_ok{false};
    auto empty = runtime.blockOnCpu(runEmpty(&empty_ok));
    assert(empty.has_value());
    assert(empty_ok.load(std::memory_order_acquire));

    runtime.stop();

    Runtime no_parallel = RuntimeBuilder().ioSchedulerCount(1).parallelSchedulerCount(0).build();
    std::atomic<bool> scheduler_rejected{false};
    auto rejected = no_parallel.blockOnIO(runWithoutParallelScheduler(&scheduler_rejected));
    assert(rejected.has_value());
    assert(scheduler_rejected.load(std::memory_order_acquire));
    no_parallel.stop();

    std::cout << "T181-ParallelDag PASS\n";
    return 0;
}
