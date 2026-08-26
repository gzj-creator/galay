/**
 * @file b34_coroutine_frame_allocator_pressure.cc
 * @brief 协程帧 recycler 的固定规模压力与 A/B 数据采集。
 */

#include <galay/cpp/galay-kernel/core/task.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <iomanip>
#include <iostream>
#include <thread>
#include <vector>

using galay::kernel::Task;
using galay::kernel::detail::allocateFrameStorage;
using galay::kernel::detail::frameFreeListSizeForTesting;
using galay::kernel::detail::setFrameRecyclerEnabledForTesting;
using galay::kernel::detail::releaseFrameStorage;

namespace {

constexpr std::size_t kAlignment = alignof(std::max_align_t);

struct Measurement {
    std::size_t iterations = 0;
    std::chrono::steady_clock::duration elapsed{};
    std::size_t fallbackEstimate = 0;
};

double nanosecondsPerOperation(const Measurement& measurement) {
    if (measurement.iterations == 0) {
        return 0.0;
    }
    return std::chrono::duration<double, std::nano>(measurement.elapsed).count() /
        static_cast<double>(measurement.iterations);
}

void printMeasurement(const char* name, const Measurement& measurement) {
    const double ns = nanosecondsPerOperation(measurement);
    const double throughput = ns > 0.0 ? 1'000'000'000.0 / ns : 0.0;
    std::cout << name << " iterations=" << measurement.iterations
              << ", elapsed_ns="
              << std::chrono::duration_cast<std::chrono::nanoseconds>(
                     measurement.elapsed)
                     .count()
              << ", ns_per_op=" << std::fixed << std::setprecision(2) << ns
              << ", ops_per_sec=" << throughput
              << ", fallback_estimate=" << measurement.fallbackEstimate << '\n';
}

Measurement measureFrameChurn(std::size_t size,
                              std::size_t iterations,
                              bool recyclerEnabled) {
    setFrameRecyclerEnabledForTesting(recyclerEnabled);
    Measurement measurement{.iterations = iterations};
    if (!recyclerEnabled || size > 2048) {
        measurement.fallbackEstimate = iterations;
    } else if (frameFreeListSizeForTesting(size, kAlignment) == 0) {
        // A same-thread size-class churn can miss the recycler only on the
        // first allocation once each release returns its node to the bucket.
        measurement.fallbackEstimate = 1;
    }
    const auto begin = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < iterations; ++i) {
        void* frame = allocateFrameStorage(size, kAlignment);
        if (frame == nullptr) {
            measurement.iterations = i;
            measurement.elapsed = std::chrono::steady_clock::now() - begin;
            return measurement;
        }
        releaseFrameStorage(frame, size, kAlignment);
    }
    measurement.elapsed = std::chrono::steady_clock::now() - begin;
    return measurement;
}

template <std::size_t Payload>
Task<void> payloadTask() {
    std::array<std::byte, Payload> payload{};
    co_await std::suspend_always{};
    (void)payload;
}

Task<void> lightweightTask() {
    co_return;
}

Task<int> integerTask() {
    co_return 1;
}

Task<void> suspendResumeTask() {
    co_await std::suspend_always{};
    co_return;
}

template <typename Factory>
Measurement measureTaskCreateDestroy(Factory&& factory, std::size_t iterations) {
    Measurement measurement{.iterations = iterations};
    const auto begin = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < iterations; ++i) {
        auto task = factory();
        if (!task.isValid()) {
            measurement.iterations = i;
            break;
        }
    }
    measurement.elapsed = std::chrono::steady_clock::now() - begin;
    return measurement;
}

Measurement measureSuspendResume(std::size_t iterations) {
    Measurement measurement{.iterations = iterations};
    const auto begin = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < iterations; ++i) {
        auto task = suspendResumeTask();
        if (!task.isValid()) {
            measurement.iterations = i;
            break;
        }
        auto* state = galay::kernel::detail::TaskAccess::taskRef(task).state();
        state->m_handle.resume();
        state->m_handle.resume();
    }
    measurement.elapsed = std::chrono::steady_clock::now() - begin;
    return measurement;
}

Measurement measureCrossThreadRelease(std::size_t iterations) {
    setFrameRecyclerEnabledForTesting(true);
    std::vector<void*> frames(iterations, nullptr);
    Measurement measurement{
        .iterations = iterations,
        .fallbackEstimate = iterations,
    };
    const auto begin = std::chrono::steady_clock::now();

    std::thread producer([&frames]() {
        setFrameRecyclerEnabledForTesting(true);
        for (void*& frame : frames) {
            frame = allocateFrameStorage(256, kAlignment);
        }
    });
    producer.join();

    std::thread consumer([&frames]() {
        setFrameRecyclerEnabledForTesting(true);
        for (void* frame : frames) {
            releaseFrameStorage(frame, 256, kAlignment);
        }
    });
    consumer.join();

    measurement.elapsed = std::chrono::steady_clock::now() - begin;
    return measurement;
}

bool measureRetainedMemory(std::size_t retainedTasks) {
    std::vector<Task<void>> tasks;
    tasks.reserve(retainedTasks);
    for (std::size_t i = 0; i < retainedTasks; ++i) {
        tasks.push_back(payloadTask<256>());
        if (!tasks.back().isValid()) {
            return false;
        }
    }
    std::cout << "retained_memory tasks=" << tasks.size()
              << ", frame_bucket_512="
              << frameFreeListSizeForTesting(512, kAlignment) << '\n';
    tasks.clear();
    return true;
}

}  // namespace

int main() {
    constexpr std::size_t kWarmupIterations = 2'000;
    constexpr std::size_t kMeasuredIterations = 20'000;

    (void)measureFrameChurn(256, kWarmupIterations, true);
    (void)measureFrameChurn(256, kWarmupIterations, false);
    setFrameRecyclerEnabledForTesting(true);

    const auto recycler =
        measureFrameChurn(256, kMeasuredIterations, true);
    const auto globalFallback =
        measureFrameChurn(256, kMeasuredIterations, false);
    printMeasurement("frame_churn_recycler", recycler);
    printMeasurement("frame_churn_disabled", globalFallback);

    const auto size128 = measureFrameChurn(128, kMeasuredIterations, true);
    const auto size512 = measureFrameChurn(512, kMeasuredIterations, true);
    const auto size2048 = measureFrameChurn(2048, kMeasuredIterations, true);
    const auto sizeLarge = measureFrameChurn(4096, kMeasuredIterations, true);
    printMeasurement("frame_size_128", size128);
    printMeasurement("frame_size_512", size512);
    printMeasurement("frame_size_2048", size2048);
    printMeasurement("frame_size_4096", sizeLarge);

    const auto voidChurn = measureTaskCreateDestroy(
        []() { return lightweightTask(); }, kMeasuredIterations);
    const auto intChurn = measureTaskCreateDestroy(
        []() { return integerTask(); }, kMeasuredIterations);
    printMeasurement("task_void_create_destroy", voidChurn);
    printMeasurement("task_int_create_destroy", intChurn);

    const auto suspendResume = measureSuspendResume(kMeasuredIterations);
    printMeasurement("task_suspend_resume", suspendResume);

    const auto crossThread = measureCrossThreadRelease(kMeasuredIterations);
    printMeasurement("cross_thread_release", crossThread);

    if (!measureRetainedMemory(5'000)) {
        std::cerr << "[B34] retained task setup failed\n";
        return 1;
    }

    setFrameRecyclerEnabledForTesting(true);
    return 0;
}
