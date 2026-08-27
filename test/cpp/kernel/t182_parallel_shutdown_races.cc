/**
 * @file t182_parallel_shutdown_races.cc
 * @brief 并行工作接纳和停机期间 parent 唤醒的回归覆盖。
 */

#include <galay/cpp/galay-kernel/core/runtime.h>
#include <galay/cpp/galay-kernel/parallel/parallel.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

using namespace galay::kernel;
using namespace std::chrono_literals;

namespace {

struct WorkCounters {
    std::atomic<std::size_t> executed{0};
    std::atomic<std::size_t> released{0};
};

void runWork(void* context, std::size_t) noexcept
{
    static_cast<WorkCounters*>(context)->executed.fetch_add(
        1, std::memory_order_release);
}

void releaseWork(void* context) noexcept
{
    static_cast<WorkCounters*>(context)->released.fetch_add(
        1, std::memory_order_release);
}

ParallelWorkItem makeWork(WorkCounters* counters)
{
    return ParallelWorkItem(counters, 0, &runWork, &releaseWork);
}

struct Blocker {
    std::atomic<bool> started{false};
    std::atomic<bool> release{false};
};

void runBlocker(void* context, std::size_t) noexcept
{
    auto* blocker = static_cast<Blocker*>(context);
    blocker->started.store(true, std::memory_order_release);
    while (!blocker->release.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
}

void releaseBlocker(void*) noexcept {}

bool verifyWorkAdmissionRace()
{
    constexpr std::size_t kRounds = 8;
    constexpr std::size_t kProducers = 16;
    constexpr std::size_t kAttemptsPerProducer = 128;

    for (std::size_t round = 0; round < kRounds; ++round) {
        ParallelScheduler scheduler;
        if (!scheduler.start().has_value()) {
            std::cerr << "[T182] scheduler start failed\n";
            return false;
        }

        Blocker blocker;
        if (!scheduler.scheduleWork(
                ParallelWorkItem(&blocker, 0, &runBlocker, &releaseBlocker))) {
            std::cerr << "[T182] blocker was rejected\n";
            scheduler.stop();
            return false;
        }
        const auto blocker_deadline = std::chrono::steady_clock::now() + 1s;
        while (!blocker.started.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < blocker_deadline) {
            std::this_thread::yield();
        }
        if (!blocker.started.load(std::memory_order_acquire)) {
            std::cerr << "[T182] blocker did not start\n";
            blocker.release.store(true, std::memory_order_release);
            scheduler.stop();
            return false;
        }

        WorkCounters counters;
        std::atomic<bool> produce{true};
        std::atomic<std::size_t> accepted{0};
        std::vector<std::thread> producers;
        producers.reserve(kProducers);
        for (std::size_t i = 0; i < kProducers; ++i) {
            producers.emplace_back([&]() {
                for (std::size_t attempt = 0;
                     attempt < kAttemptsPerProducer &&
                     produce.load(std::memory_order_acquire);
                     ++attempt) {
                    if (scheduler.scheduleWork(makeWork(&counters))) {
                        accepted.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            });
        }

        std::this_thread::sleep_for(1ms);
        std::thread stopper([&]() { scheduler.stop(); });
        while (scheduler.isRunning()) {
            std::this_thread::yield();
        }
        produce.store(false, std::memory_order_release);
        blocker.release.store(true, std::memory_order_release);
        stopper.join();
        for (auto& producer : producers) {
            producer.join();
        }

        const auto accepted_count = accepted.load(std::memory_order_acquire);
        const auto executed_count = counters.executed.load(std::memory_order_acquire);
        if (accepted_count != executed_count) {
            std::cerr << "[T182] accepted work was not executed during stop: accepted="
                      << accepted_count << " executed=" << executed_count
                      << " round=" << round << '\n';
            return false;
        }
    }
    return true;
}

Task<void> shutdownGraph(std::atomic<bool>* started,
                         std::atomic<bool>* release,
                         std::atomic<bool>* ran_after_await)
{
    ParallelGraph graph;
    auto first = graph.add(makeParallelWork([started, release]() noexcept {
        started->store(true, std::memory_order_release);
        while (!release->load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    }));
    auto second = graph.add(makeParallelWork([]() noexcept {}));
    if (!first.has_value() || !second.has_value() ||
        !graph.then(*first, *second).has_value()) {
        co_return;
    }

    const auto result = co_await parallel(std::move(graph));
    (void)result;
    ran_after_await->store(true, std::memory_order_release);
    co_return;
}

struct ScheduleOnDestroy {
    ParallelScheduler* scheduler = nullptr;
    WorkCounters* counters = nullptr;

    ~ScheduleOnDestroy()
    {
        if (scheduler != nullptr && counters != nullptr) {
            (void)scheduler->scheduleWork(makeWork(counters));
        }
    }
};

Task<void> resumeSchedulesWork(ParallelScheduler* scheduler,
                               WorkCounters* counters)
{
    // 析构函数在 owner worker 排空已入队的 owner-only resume 时执行；
    // 它提交的普通工作项也必须被排空。
    ScheduleOnDestroy guard{scheduler, counters};
    co_return;
}

bool verifyOwnerDrainAdmission()
{
    ParallelScheduler scheduler;
    if (!scheduler.start().has_value()) {
        std::cerr << "[T182] owner-drain scheduler start failed\n";
        return false;
    }

    Blocker blocker;
    if (!scheduler.scheduleWork(
            ParallelWorkItem(&blocker, 0, &runBlocker, &releaseBlocker))) {
        std::cerr << "[T182] owner-drain blocker was rejected\n";
        scheduler.stop();
        return false;
    }
    const auto blocker_deadline = std::chrono::steady_clock::now() + 1s;
    while (!blocker.started.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < blocker_deadline) {
        std::this_thread::yield();
    }
    if (!blocker.started.load(std::memory_order_acquire)) {
        std::cerr << "[T182] owner-drain blocker did not start\n";
        blocker.release.store(true, std::memory_order_release);
        scheduler.stop();
        return false;
    }

    WorkCounters counters;
    auto task = resumeSchedulesWork(&scheduler, &counters);
    auto task_ref = detail::TaskAccess::taskRef(task);
    auto* state = task_ref.state();
    if (state == nullptr) {
        std::cerr << "[T182] owner-drain task has no state\n";
        blocker.release.store(true, std::memory_order_release);
        scheduler.stop();
        return false;
    }
    detail::setTaskScheduler(task_ref, &scheduler);
    if (detail::requestTaskResumeStateDetailed(state) !=
        detail::TaskResumeResult::kAccepted) {
        std::cerr << "[T182] owner-drain resume was rejected\n";
        blocker.release.store(true, std::memory_order_release);
        scheduler.stop();
        return false;
    }

    std::thread stopper([&]() { scheduler.stop(); });
    while (scheduler.isRunning()) {
        std::this_thread::yield();
    }
    blocker.release.store(true, std::memory_order_release);
    stopper.join();

    const auto executed = counters.executed.load(std::memory_order_acquire);
    const auto released = counters.released.load(std::memory_order_acquire);
    if (executed != 1 || released != 1) {
        std::cerr << "[T182] owner-drain work counts mismatch: executed="
                  << executed << " released=" << released << '\n';
        return false;
    }
    return true;
}

bool verifyParentResumeFailureIsObservable()
{
    Runtime runtime = RuntimeBuilder().ioSchedulerCount(0).parallelSchedulerCount(2).build();
    std::atomic<bool> work_started{false};
    std::atomic<bool> release_work{false};
    std::atomic<bool> resumed_parent{false};

    auto task = shutdownGraph(&work_started, &release_work, &resumed_parent);
    TaskRef observer = detail::TaskAccess::taskRef(task);
    auto handle = runtime.spawnCpu(std::move(task));
    if (!handle.has_value()) {
        std::cerr << "[T182] failed to submit graph parent\n";
        runtime.stop();
        return false;
    }

    const auto started_deadline = std::chrono::steady_clock::now() + 1s;
    while (!work_started.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < started_deadline) {
        std::this_thread::yield();
    }
    if (!work_started.load(std::memory_order_acquire)) {
        std::cerr << "[T182] graph work did not start\n";
        release_work.store(true, std::memory_order_release);
        runtime.stop();
        return false;
    }

    // 根任务运行在 parallel scheduler 0，首个图节点运行在 scheduler 1。
    // 在图节点完成前停止 parent 的 owner scheduler。
    auto* parent_scheduler = runtime.getParallelScheduler(0);
    if (parent_scheduler == nullptr) {
        std::cerr << "[T182] parent scheduler missing\n";
        release_work.store(true, std::memory_order_release);
        runtime.stop();
        return false;
    }
    parent_scheduler->stop();
    release_work.store(true, std::memory_order_release);

    const auto completion_deadline = std::chrono::steady_clock::now() + 1s;
    auto* state = observer.state();
    while (state != nullptr && !state->m_done.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < completion_deadline) {
        std::this_thread::yield();
    }
    if (state == nullptr || !state->m_done.load(std::memory_order_acquire)) {
        std::cerr << "[T182] parent remained incomplete after resume failure\n";
        runtime.getParallelScheduler(1)->stop();
        runtime.stop();
        return false;
    }

    const auto joined = handle->join();
    const bool observable = !joined.has_value() &&
        joined.error().code() == detail::TaskResultErrorCode::kResumeFailed &&
        !resumed_parent.load(std::memory_order_acquire);

    runtime.getParallelScheduler(1)->stop();
    runtime.stop();
    if (!observable) {
        return false;
    }

    // 同步根任务 API 应保留更具体的错误，不能将 owner resume 失败折叠为提交失败。
    Runtime blocking_runtime = RuntimeBuilder()
        .ioSchedulerCount(0)
        .parallelSchedulerCount(2)
        .build();
    if (!blocking_runtime.start().has_value()) {
        return false;
    }
    std::atomic<bool> block_started{false};
    std::atomic<bool> block_release{false};
    std::thread stopper([&]() {
        while (!block_started.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        blocking_runtime.getParallelScheduler(0)->stop();
        block_release.store(true, std::memory_order_release);
    });
    const auto blocked = blocking_runtime.blockOnCpu(
        shutdownGraph(&block_started, &block_release, &resumed_parent));
    stopper.join();
    const bool mapped = !blocked.has_value() &&
        blocked.error().code() == RuntimeErrorCode::kResumeFailed;
    blocking_runtime.stop();
    return mapped;
}

} // namespace

int main()
{
    if (!verifyWorkAdmissionRace() || !verifyOwnerDrainAdmission() ||
        !verifyParentResumeFailureIsObservable()) {
        return 1;
    }
    std::cout << "T182-ParallelShutdownRaces PASS\n";
    return 0;
}
