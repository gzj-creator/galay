/**
 * @file b34_coroutine_frame_allocator_pressure.cc
 * @brief 固定尺寸协程 frame recycler 压力与 direct allocation 基线。
 */

#include <galay/cpp/galay-kernel/core/task.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <iomanip>
#include <iostream>
#include <new>
#include <thread>
#include <vector>

using galay::kernel::Task;
using galay::kernel::detail::allocateFrameStorage;
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

Measurement measureFrameChurn(std::size_t size, std::size_t iterations) {
    Measurement measurement{
        .iterations = iterations,
        .fallbackEstimate = size > 2048 ? iterations : 1,
    };
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

Measurement measureDirectFrameChurn(std::size_t size,
                                    std::size_t iterations) {
    Measurement measurement{
        .iterations = iterations,
        .fallbackEstimate = iterations,
    };
    const auto begin = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < iterations; ++i) {
        void* frame = ::operator new(size, std::nothrow);
        if (frame == nullptr) {
            measurement.iterations = i;
            break;
        }
        ::operator delete(frame);
    }
    measurement.elapsed = std::chrono::steady_clock::now() - begin;
    return measurement;
}

Measurement measureMixedFrameChurn(std::size_t iterations) {
    constexpr std::array<std::size_t, 6> sizes{{128, 256, 512, 1024, 2048, 4096}};
    Measurement measurement{
        .iterations = iterations,
        .fallbackEstimate = iterations / sizes.size() + sizes.size() - 1,
    };
    const auto begin = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < iterations; ++i) {
        const std::size_t size = sizes[i % sizes.size()];
        void* frame = allocateFrameStorage(size, kAlignment);
        if (frame == nullptr) {
            measurement.iterations = i;
            break;
        }
        releaseFrameStorage(frame, size, kAlignment);
    }
    measurement.elapsed = std::chrono::steady_clock::now() - begin;
    return measurement;
}

Measurement measureDirectMixedFrameChurn(std::size_t iterations) {
    constexpr std::array<std::size_t, 6> sizes{{128, 256, 512, 1024, 2048, 4096}};
    Measurement measurement{
        .iterations = iterations,
        .fallbackEstimate = iterations,
    };
    const auto begin = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < iterations; ++i) {
        void* frame = ::operator new(sizes[i % sizes.size()], std::nothrow);
        if (frame == nullptr) {
            measurement.iterations = i;
            break;
        }
        ::operator delete(frame);
    }
    measurement.elapsed = std::chrono::steady_clock::now() - begin;
    return measurement;
}

Measurement measureOverAlignedChurn(std::size_t iterations,
                                    std::size_t alignment) {
    Measurement measurement{
        .iterations = iterations,
        .fallbackEstimate = iterations,
    };
    const auto begin = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < iterations; ++i) {
        void* frame = allocateFrameStorage(256, alignment);
        if (frame == nullptr) {
            measurement.iterations = i;
            break;
        }
        releaseFrameStorage(frame, 256, alignment);
    }
    measurement.elapsed = std::chrono::steady_clock::now() - begin;
    return measurement;
}

Measurement measureLiveBurst(std::size_t size,
                             std::size_t frameCount) {
    std::vector<void*> frames(frameCount, nullptr);
    Measurement measurement{
        .iterations = frameCount,
        .fallbackEstimate = frameCount,
    };
    const auto begin = std::chrono::steady_clock::now();
    for (void*& frame : frames) {
        frame = allocateFrameStorage(size, kAlignment);
        if (frame == nullptr) {
            measurement.iterations =
                static_cast<std::size_t>(&frame - frames.data());
            break;
        }
    }
    for (void* frame : frames) {
        releaseFrameStorage(frame, size, kAlignment);
    }
    measurement.elapsed = std::chrono::steady_clock::now() - begin;
    return measurement;
}

Measurement measureConcurrentFrameChurn(std::size_t iterationsPerWorker,
                                         std::size_t workerCount) {
    constexpr std::array<std::size_t, 4> sizes{{128, 256, 512, 2048}};
    std::atomic<std::size_t> completed{0};
    std::vector<std::thread> workers;
    workers.reserve(workerCount);
    const auto begin = std::chrono::steady_clock::now();
    for (std::size_t worker = 0; worker < workerCount; ++worker) {
        workers.emplace_back([&, worker]() {
            std::size_t localCompleted = 0;
            for (std::size_t i = 0; i < iterationsPerWorker; ++i) {
                const std::size_t size = sizes[(i + worker) % sizes.size()];
                void* frame = allocateFrameStorage(size, kAlignment);
                if (frame == nullptr) {
                    continue;
                }
                releaseFrameStorage(frame, size, kAlignment);
                ++localCompleted;
            }
            completed.fetch_add(localCompleted, std::memory_order_relaxed);
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }

    Measurement measurement{
        .iterations = completed.load(std::memory_order_relaxed),
        .fallbackEstimate = workerCount * sizes.size(),
    };
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

template <typename Factory>
Measurement measureTaskCompleteDestroy(Factory&& factory,
                                       std::size_t iterations) {
    Measurement measurement{.iterations = iterations};
    const auto begin = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < iterations; ++i) {
        auto task = factory();
        if (!task.isValid()) {
            measurement.iterations = i;
            break;
        }
        auto* state = galay::kernel::detail::TaskAccess::taskRef(task).state();
        if (state == nullptr || state->m_handle == nullptr) {
            measurement.iterations = i;
            break;
        }
        state->m_handle.resume();
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
    std::vector<void*> frames(iterations, nullptr);
    Measurement measurement{
        .iterations = iterations,
        .fallbackEstimate = iterations,
    };
    const auto begin = std::chrono::steady_clock::now();

    std::thread producer([&frames]() {
        for (void*& frame : frames) {
            frame = allocateFrameStorage(256, kAlignment);
        }
    });
    producer.join();

    std::thread consumer([&frames]() {
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
    std::cout << "retained_memory tasks=" << tasks.size() << '\n';
    tasks.clear();
    std::cout << "retained_memory_after_clear tasks=0\n";
    return true;
}

}  // namespace

int main() {
    constexpr std::size_t kWarmupIterations = 2'000;
    constexpr std::size_t kMeasuredIterations = 20'000;

    (void)measureFrameChurn(256, kWarmupIterations);

    const auto recycler =
        measureFrameChurn(256, kMeasuredIterations);
    const auto direct =
        measureDirectFrameChurn(256, kMeasuredIterations);
    printMeasurement("frame_churn_recycler", recycler);
    printMeasurement("frame_churn_direct", direct);

    const auto size128 = measureFrameChurn(128, kMeasuredIterations);
    const auto size512 = measureFrameChurn(512, kMeasuredIterations);
    const auto size2048 = measureFrameChurn(2048, kMeasuredIterations);
    const auto sizeLarge = measureFrameChurn(4096, kMeasuredIterations);
    printMeasurement("frame_size_128", size128);
    printMeasurement("frame_size_512", size512);
    printMeasurement("frame_size_2048", size2048);
    printMeasurement("frame_size_4096", sizeLarge);

    const auto mixedRecycler =
        measureMixedFrameChurn(kMeasuredIterations);
    const auto mixedDirect =
        measureDirectMixedFrameChurn(kMeasuredIterations);
    printMeasurement("frame_mixed_recycler", mixedRecycler);
    printMeasurement("frame_mixed_direct", mixedDirect);

    const auto overAligned64 =
        measureOverAlignedChurn(kMeasuredIterations, kAlignment * 4);
    printMeasurement("frame_overaligned_64", overAligned64);

    const auto liveBurst = measureLiveBurst(128, 4096);
    printMeasurement("frame_live_burst_128", liveBurst);

    const auto concurrentRecycler =
        measureConcurrentFrameChurn(50'000, 4);
    printMeasurement("frame_concurrent_recycler", concurrentRecycler);
    if (mixedRecycler.iterations != kMeasuredIterations ||
        mixedDirect.iterations != kMeasuredIterations ||
        overAligned64.iterations != kMeasuredIterations ||
        liveBurst.iterations != 4096 ||
        concurrentRecycler.iterations != 200'000) {
        std::cerr << "[B34] frame pressure setup or allocation failed\n";
        return 1;
    }

    const auto voidChurn = measureTaskCreateDestroy(
        []() { return lightweightTask(); }, kMeasuredIterations);
    const auto intChurn = measureTaskCreateDestroy(
        []() { return integerTask(); }, kMeasuredIterations);
    printMeasurement("task_void_unsubmitted_destroy", voidChurn);
    printMeasurement("task_int_unsubmitted_destroy", intChurn);

    const auto voidComplete = measureTaskCompleteDestroy(
        []() { return lightweightTask(); }, kMeasuredIterations);
    const auto intComplete = measureTaskCompleteDestroy(
        []() { return integerTask(); }, kMeasuredIterations);
    printMeasurement("task_void_complete_destroy", voidComplete);
    printMeasurement("task_int_complete_destroy", intComplete);

    const auto suspendResume = measureSuspendResume(kMeasuredIterations);
    printMeasurement("task_suspend_resume", suspendResume);

    const auto crossThread = measureCrossThreadRelease(kMeasuredIterations);
    printMeasurement("cross_thread_release", crossThread);

    if (recycler.iterations != kMeasuredIterations ||
        direct.iterations != kMeasuredIterations ||
        size128.iterations != kMeasuredIterations ||
        size512.iterations != kMeasuredIterations ||
        size2048.iterations != kMeasuredIterations ||
        sizeLarge.iterations != kMeasuredIterations ||
        voidChurn.iterations != kMeasuredIterations ||
        intChurn.iterations != kMeasuredIterations ||
        voidComplete.iterations != kMeasuredIterations ||
        intComplete.iterations != kMeasuredIterations ||
        suspendResume.iterations != kMeasuredIterations ||
        crossThread.iterations != kMeasuredIterations) {
        std::cerr << "[B34] task pressure setup or allocation failed\n";
        return 1;
    }

    if (!measureRetainedMemory(5'000)) {
        std::cerr << "[B34] retained task setup failed\n";
        return 1;
    }

    return 0;
}
