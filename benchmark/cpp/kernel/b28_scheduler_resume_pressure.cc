/**
 * @file b28_scheduler_resume_pressure.cc
 * @brief 测量 ParallelScheduler 连续恢复和 IO scheduler 批量恢复搬运成本。
 */

#include "../common/benchmark_environment.h"

#include <galay/cpp/galay-kernel/parallel/parallel_scheduler.h>
#include <galay/cpp/galay-kernel/core/io_scheduler.hpp>
#include <galay/cpp/galay-kernel/core/waker.h>

#include "benchmark/cpp/common/micro_measurement.h"

#include <algorithm>
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
using galay::benchmark::MicroMeasurement;
using galay::benchmark::MicroTimer;

struct SelfWakeAwaitable {
    bool await_ready() const noexcept { return false; }

    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> handle) const noexcept {
        Waker(handle).wake_up();
        return true;
    }

    void await_resume() const noexcept {}
};

Task<void> run_self_wake(std::atomic<bool>* done, size_t iterations, MicroMeasurement* measured) {
    MicroTimer timer;
    for (size_t i = 0; i < iterations; ++i) {
        co_await SelfWakeAwaitable{};
    }
    timer.finish(*measured);
    done->store(true, std::memory_order_release);
    co_return;
}

std::optional<MicroMeasurement> measure_compute_resume(size_t iterations) {
    ParallelScheduler scheduler;
    const auto started = scheduler.start();
    if (!started.has_value()) {
        return std::nullopt;
    }

    std::atomic<bool> done{false};
    MicroMeasurement measured{.iterations = iterations};
    const auto begin = std::chrono::steady_clock::now();
    const bool scheduled = scheduler.schedule(
        detail::TaskAccess::detach_task(run_self_wake(&done, iterations, &measured)));
    if (!scheduled) {
        scheduler.stop();
        return std::nullopt;
    }

    const auto deadline = begin + std::chrono::seconds(30);
    while (!done.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        // The synchronous observer must not consume a CPU beside the worker.
        // Worker timestamps exclude this polling interval from resume latency.
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    scheduler.stop();
    if (!done.load(std::memory_order_acquire)) {
        return std::nullopt;
    }
    return measured;
}

template <size_t BatchSize>
std::optional<MicroMeasurement> measure_io_resume_drain(size_t repetitions) {
    IOReadyQueue worker;
    worker.set_stealing_enabled(false);
    std::array<TaskRef, BatchSize> tasks;
    for (TaskRef& task : tasks) {
        task = TaskRef(new TaskState(std::coroutine_handle<>{}), false);
    }

    MicroMeasurement measured{.iterations = repetitions * BatchSize};
    MicroTimer timer;
    for (size_t repetition = 0; repetition < repetitions; ++repetition) {
        for (const TaskRef& task : tasks) {
            if (!worker.schedule_resume(task).has_value()) {
                return std::nullopt;
            }
        }
        if (worker.drain_injected() != BatchSize) {
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
    timer.finish(measured);
    return measured;
}

void report(const char* name, const char* metric, const MicroMeasurement& measured, size_t batch = 0) {
    const double elapsed = std::chrono::duration<double, std::nano>(measured.elapsed).count();
    std::cout << name << " iterations=" << measured.iterations
              << " elapsed_ns=" << std::fixed << std::setprecision(2) << elapsed
              << " cpu_ns=" << measured.cpu_ns << ' ' << metric << '='
              << elapsed / measured.iterations;
    if (batch) { std::cout << " batch=" << batch; }
    std::cout << '\n';
}

template <size_t BatchSize>
bool report_io_resume_drain(size_t iterations) {
    const auto repetitions = (iterations + BatchSize - 1) / BatchSize;
    const auto warmup = measure_io_resume_drain<BatchSize>(std::max(size_t(1), repetitions / 10));
    if (!warmup || warmup->cpu_ns < 0) { return false; }
    const auto measured = measure_io_resume_drain<BatchSize>(repetitions);
    if (!measured || measured->cpu_ns < 0) { return false; }
    report("IOSchedulerResumeDrain", "ns_per_task", *measured, BatchSize);
    return true;
}
} // namespace

int main(int argc, char** argv) {
    if (!galay::benchmark::initialize_benchmark_environment()) {
        return 1;
    }

    galay::benchmark::MicroOptions options;
    if (!galay::benchmark::parse_micro_options(argc, argv, options) || options.diagnostics ||
        (!options.sample.empty() && options.sample != "parallel" && options.sample != "io_1" &&
         options.sample != "io_8" && options.sample != "io_64" && options.sample != "io_256")) {
        std::cerr << "B28: --sample parallel|io_1|io_8|io_64|io_256 --iterations N\n";
        return 1;
    }
    const auto selected = [&](std::string_view name) { return options.sample.empty() || options.sample == name; };
    if (selected("parallel")) {
        const auto warmup = measure_compute_resume(std::max(size_t(1), options.iterations / 10));
        if (!warmup || warmup->cpu_ns < 0) { return 1; }
        const auto measured = measure_compute_resume(options.iterations);
        if (!measured || measured->cpu_ns < 0) { return 1; }
        report("ParallelSchedulerResume", "ns_per_resume", *measured);
    }
    if ((selected("io_1") && !report_io_resume_drain<1>(options.iterations)) ||
        (selected("io_8") && !report_io_resume_drain<8>(options.iterations)) ||
        (selected("io_64") && !report_io_resume_drain<64>(options.iterations)) ||
        (selected("io_256") && !report_io_resume_drain<256>(options.iterations))) { return 1; }
    return 0;
}
