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
#include <thread>
#include <vector>

using namespace galay::kernel;

namespace {

bool require(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "[T181] " << message << '\n';
    }
    return condition;
}

bool verifyGraphInputErrors()
{
    ParallelGraph graph;
    const auto first = graph.add(makeParallelWork([]() noexcept {}));
    const auto second = graph.add(makeParallelWork([]() noexcept {}));
    if (!require(first.has_value() && second.has_value(),
                 "graph setup failed")) {
        return false;
    }

    const auto invalid = graph.then(ParallelGraph::kInvalidNode, *first);
    if (!require(!invalid.has_value() &&
                     invalid.error().code() == ParallelErrorCode::kInvalidNode,
                 "invalid node must return kInvalidNode")) {
        return false;
    }
    const auto self = graph.then(*first, *first);
    if (!require(!self.has_value() &&
                     self.error().code() == ParallelErrorCode::kInvalidNode,
                 "self dependency must return kInvalidNode")) {
        return false;
    }
    const auto edge = graph.then(*first, *second);
    if (!require(edge.has_value(), "valid dependency was rejected")) {
        return false;
    }
    const auto duplicate = graph.then(*first, *second);
    return require(!duplicate.has_value() &&
                        duplicate.error().code() ==
                            ParallelErrorCode::kDuplicateDependency,
                    "duplicate dependency must return kDuplicateDependency");
}

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

Task<void> runInlineFailure(std::atomic<bool>* ran,
                            std::atomic<bool>* observed)
{
    ParallelGraph graph;
    auto node = graph.add(makeParallelWork([ran]() noexcept
        -> std::expected<void, ParallelError> {
        ran->store(true, std::memory_order_release);
        return std::unexpected(ParallelError(ParallelErrorCode::kWorkFailed));
    }));
    assert(node.has_value());
    const auto result = co_await parallel(std::move(graph));
    observed->store(!result.has_value() &&
                        result.error().code() == ParallelErrorCode::kWorkFailed &&
                        result.error().node() == 0,
                    std::memory_order_release);
    co_return;
}

Task<void> runFirstError(std::atomic<bool>* fast_failed,
                         std::atomic<std::size_t>* error_node,
                         std::atomic<bool>* ok)
{
    ParallelGraph graph;
    auto fast = graph.add(makeParallelWork([fast_failed]() noexcept
        -> std::expected<void, ParallelError> {
        fast_failed->store(true, std::memory_order_release);
        return std::unexpected(ParallelError(ParallelErrorCode::kWorkFailed));
    }));
    auto slow = graph.add(makeParallelWork([fast_failed]() noexcept
        -> std::expected<void, ParallelError> {
        while (!fast_failed->load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        return std::unexpected(ParallelError(ParallelErrorCode::kWorkFailed));
    }));
    assert(slow.has_value() && fast.has_value());

    const auto result = co_await parallel(std::move(graph));
    ok->store(!result.has_value() &&
                  result.error().code() == ParallelErrorCode::kWorkFailed,
              std::memory_order_release);
    if (!result.has_value()) {
        error_node->store(result.error().node(), std::memory_order_release);
    }
    co_return;
}

Task<void> runScheduleFailure(std::atomic<bool>* observed)
{
    ParallelGraph graph;
    auto first = graph.add(makeParallelWork([]() noexcept {}));
    auto second = graph.add(makeParallelWork([]() noexcept {}));
    assert(first.has_value() && second.has_value());
    assert(graph.then(*first, *second).has_value());
    const auto result = co_await parallel(std::move(graph));
    observed->store(!result.has_value() &&
                        result.error().code() == ParallelErrorCode::kScheduleFailed &&
                        result.error().node() == 0,
                    std::memory_order_release);
    co_return;
}

Task<void> runEmpty(std::atomic<bool>* ok)
{
    const auto result = co_await parallel(ParallelGraph{});
    ok->store(result.has_value(), std::memory_order_release);
    co_return;
}

Task<void> runLargeReverseChain(std::atomic<bool>* ok)
{
    constexpr std::size_t kNodes = 2048;
    ParallelGraph graph;
    std::vector<ParallelNodeId> nodes;
    nodes.reserve(kNodes);
    for (std::size_t index = 0; index < kNodes; ++index) {
        auto node = graph.add(makeParallelWork([]() noexcept {}));
        if (!node.has_value()) {
            ok->store(false, std::memory_order_release);
            co_return;
        }
        nodes.push_back(*node);
    }
    // Reverse the edge direction so a repeated full scan needs one pass per
    // node. This keeps the validation boundary covered by a non-trivial DAG.
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
    assert(verifyGraphInputErrors());

    // Both failures are submitted to one worker in FIFO order. This keeps the
    // assertion about the first error deterministic while the other tests
    // continue to exercise multi-scheduler execution.
    Runtime first_error_runtime = RuntimeBuilder()
        .ioSchedulerCount(0)
        .parallelSchedulerCount(1)
        .build();
    std::atomic<bool> fast_failed{false};
    std::atomic<std::size_t> first_error_node{ParallelGraph::kInvalidNode};
    std::atomic<bool> first_error_ok{false};
    auto first_error = first_error_runtime.blockOnCpu(
        runFirstError(&fast_failed, &first_error_node, &first_error_ok));
    assert(first_error.has_value());
    assert(first_error_ok.load(std::memory_order_acquire));
    assert(first_error_node.load(std::memory_order_acquire) == 0);
    first_error_runtime.stop();

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

    std::atomic<bool> inline_ran{false};
    std::atomic<bool> inline_failure_observed{false};
    auto inline_failure = runtime.blockOnCpu(
        runInlineFailure(&inline_ran, &inline_failure_observed));
    assert(inline_failure.has_value());
    assert(inline_ran.load(std::memory_order_acquire));
    assert(inline_failure_observed.load(std::memory_order_acquire));

    std::atomic<bool> empty_ok{false};
    auto empty = runtime.blockOnCpu(runEmpty(&empty_ok));
    assert(empty.has_value());
    assert(empty_ok.load(std::memory_order_acquire));

    std::atomic<bool> large_chain_ok{false};
    auto large_chain = runtime.blockOnCpu(runLargeReverseChain(&large_chain_ok));
    assert(large_chain.has_value());
    assert(large_chain_ok.load(std::memory_order_acquire));

    runtime.stop();

    Runtime no_parallel = RuntimeBuilder().ioSchedulerCount(1).parallelSchedulerCount(0).build();
    std::atomic<bool> scheduler_rejected{false};
    auto rejected = no_parallel.blockOnIO(runWithoutParallelScheduler(&scheduler_rejected));
    assert(rejected.has_value());
    assert(scheduler_rejected.load(std::memory_order_acquire));
    no_parallel.stop();

    Runtime stopped_parallel = RuntimeBuilder()
        .ioSchedulerCount(0)
        .parallelSchedulerCount(2)
        .build();
    assert(stopped_parallel.start().has_value());
    // The root uses scheduler 0; the first graph node is selected on
    // scheduler 1, which is stopped before submission.
    stopped_parallel.getParallelScheduler(1)->stop();
    std::atomic<bool> schedule_failure_observed{false};
    auto schedule_failure = stopped_parallel.spawnCpu(
        runScheduleFailure(&schedule_failure_observed));
    assert(schedule_failure.has_value());
    const auto joined_schedule_failure = schedule_failure->join();
    assert(joined_schedule_failure.has_value());
    assert(schedule_failure_observed.load(std::memory_order_acquire));
    stopped_parallel.stop();

    std::cout << "T181-ParallelDag PASS\n";
    return 0;
}
