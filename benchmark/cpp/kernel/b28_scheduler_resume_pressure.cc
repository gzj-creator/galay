/**
 * @file b28_scheduler_resume_pressure.cc
 * @brief 测量 ComputeScheduler 连续恢复和 IO scheduler 批量恢复搬运成本。
 */

#include <galay/cpp/galay-kernel/core/compute_scheduler.h>
#include <galay/cpp/galay-kernel/core/io_scheduler.hpp>
#include <galay/cpp/galay-kernel/core/waker.h>

#include <array>
#include <atomic>
#include <chrono>
#include <coroutine>
#include <cstddef>
#include <iomanip>
#include <iostream>
#include <optional>
#include <thread>

using namespace galay::kernel;

namespace {

struct SelfWakeAwaitable {
    bool await_ready() const noexcept { return false; }

    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> handle) const noexcept {
        Waker(handle).wakeUp();
        return true;
    }

    void await_resume() const noexcept {}
};

Task<void> runSelfWake(std::atomic<bool>* done, size_t iterations) {
    for (size_t i = 0; i < iterations; ++i) {
        co_await SelfWakeAwaitable{};
    }
    done->store(true, std::memory_order_release);
    co_return;
}

std::optional<double> measureComputeResume(size_t iterations) {
    ComputeScheduler scheduler;
    const auto started = scheduler.start();
    if (!started.has_value()) {
        return std::nullopt;
    }

    std::atomic<bool> done{false};
    const auto begin = std::chrono::steady_clock::now();
    const bool scheduled = scheduler.schedule(
        detail::TaskAccess::detachTask(runSelfWake(&done, iterations)));
    if (!scheduled) {
        scheduler.stop();
        return std::nullopt;
    }

    const auto deadline = begin + std::chrono::seconds(30);
    while (!done.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    const auto end = std::chrono::steady_clock::now();
    scheduler.stop();
    if (!done.load(std::memory_order_acquire)) {
        return std::nullopt;
    }
    return std::chrono::duration<double, std::nano>(end - begin).count() /
        static_cast<double>(iterations);
}

template <size_t BatchSize>
std::optional<double> measureIOResumeDrain(size_t repetitions) {
    IOSchedulerWorkerState worker;
    worker.setStealingEnabled(false);
    std::array<TaskRef, BatchSize> tasks;
    for (TaskRef& task : tasks) {
        task = TaskRef(new TaskState(std::coroutine_handle<>{}), false);
    }

    const auto begin = std::chrono::steady_clock::now();
    for (size_t repetition = 0; repetition < repetitions; ++repetition) {
        for (const TaskRef& task : tasks) {
            if (!worker.scheduleResume(task).has_value()) {
                return std::nullopt;
            }
        }
        if (worker.drainInjected() != BatchSize) {
            return std::nullopt;
        }
        for (size_t i = 0; i < BatchSize; ++i) {
            TaskRef popped;
            if (!worker.local_ring.pop_back(popped)) {
                return std::nullopt;
            }
            popped.state()->m_resume_queue_claimed.store(
                false, std::memory_order_release);
        }
    }
    const auto end = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::nano>(end - begin).count() /
        static_cast<double>(repetitions * BatchSize);
}

template <size_t BatchSize>
bool reportIOResumeDrain(size_t repetitions) {
    const auto warmup = measureIOResumeDrain<BatchSize>(repetitions / 10);
    if (!warmup.has_value()) {
        return false;
    }
    const auto measured = measureIOResumeDrain<BatchSize>(repetitions);
    if (!measured.has_value()) {
        return false;
    }
    std::cout << "IOSchedulerResumeDrain batch=" << BatchSize
              << ", ns_per_task=" << std::fixed << std::setprecision(2)
              << *measured << '\n';
    return true;
}

}  // namespace

int main() {
    constexpr size_t kComputeWarmupIterations = 1'000;
    constexpr size_t kComputeIterations = 10'000;
    const auto compute_warmup = measureComputeResume(kComputeWarmupIterations);
    const auto compute = measureComputeResume(kComputeIterations);
    if (!compute_warmup.has_value() || !compute.has_value()) {
        std::cerr << "[B28] ComputeScheduler resume benchmark failed\n";
        return 1;
    }
    std::cout << "ComputeSchedulerResume iterations=" << kComputeIterations
              << ", ns_per_resume="
              << std::fixed << std::setprecision(2) << *compute << '\n';

    if (!reportIOResumeDrain<1>(200'000) ||
        !reportIOResumeDrain<8>(50'000) ||
        !reportIOResumeDrain<64>(10'000) ||
        !reportIOResumeDrain<256>(2'500)) {
        std::cerr << "[B28] IOScheduler resume benchmark failed\n";
        return 1;
    }
    return 0;
}
