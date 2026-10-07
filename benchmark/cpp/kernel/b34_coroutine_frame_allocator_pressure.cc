/**
 * @file b34_coroutine_frame_allocator_pressure.cc
 * @brief 固定尺寸协程 frame recycler 压力与 direct allocation 基线。
 */

#include "../common/benchmark_environment.h"

#include <galay/cpp/galay-kernel/core/task.h>

#include "benchmark/cpp/common/micro_measurement.h"

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
using galay::kernel::detail::allocate_frame_storage;
using galay::kernel::detail::release_frame_storage;

namespace {

constexpr std::size_t kAlignment = alignof(std::max_align_t);

using Measurement = galay::benchmark::MicroMeasurement;
using galay::benchmark::MicroTimer;

double nanoseconds_per_operation(const Measurement& measurement) {
    if (measurement.iterations == 0) {
        return 0.0;
    }
    return std::chrono::duration<double, std::nano>(measurement.elapsed).count() /
        static_cast<double>(measurement.iterations);
}

void print_measurement(const char* name, const Measurement& measurement) {
    const double ns = nanoseconds_per_operation(measurement);
    const double throughput = ns > 0.0 ? 1'000'000'000.0 / ns : 0.0;
    std::cout << name << " iterations=" << measurement.iterations
              << ", elapsed_ns="
              << std::chrono::duration_cast<std::chrono::nanoseconds>(
                     measurement.elapsed)
                     .count()
              << ", cpu_ns=" << measurement.cpu_ns
              << ", ns_per_op=" << std::fixed << std::setprecision(2) << ns
              << ", ops_per_sec=" << throughput
              << ", fallback_estimate=" << measurement.fallbackEstimate << '\n';
}

Measurement measure_frame_churn(std::size_t size, std::size_t iterations) {
    Measurement measurement{
        .iterations = iterations,
        .fallbackEstimate = size > 2048 ? iterations : 1,
    };
    MicroTimer timer;
    for (std::size_t i = 0; i < iterations; ++i) {
        void* frame = allocate_frame_storage(size, kAlignment);
        if (frame == nullptr) {
            measurement.iterations = i;
            timer.finish(measurement);
            return measurement;
        }
        release_frame_storage(frame, size, kAlignment);
    }
    timer.finish(measurement);
    return measurement;
}

Measurement measure_direct_frame_churn(std::size_t size,
                                    std::size_t iterations) {
    Measurement measurement{
        .iterations = iterations,
        .fallbackEstimate = iterations,
    };
    MicroTimer timer;
    for (std::size_t i = 0; i < iterations; ++i) {
        void* frame = ::operator new(size, std::nothrow);
        if (frame == nullptr) {
            measurement.iterations = i;
            break;
        }
        ::operator delete(frame);
    }
    timer.finish(measurement);
    return measurement;
}

Measurement measure_mixed_frame_churn(std::size_t iterations) {
    constexpr std::array<std::size_t, 6> sizes{{128, 256, 512, 1024, 2048, 4096}};
    Measurement measurement{
        .iterations = iterations,
        .fallbackEstimate = iterations / sizes.size() + sizes.size() - 1,
    };
    MicroTimer timer;
    for (std::size_t i = 0; i < iterations; ++i) {
        const std::size_t size = sizes[i % sizes.size()];
        void* frame = allocate_frame_storage(size, kAlignment);
        if (frame == nullptr) {
            measurement.iterations = i;
            break;
        }
        release_frame_storage(frame, size, kAlignment);
    }
    timer.finish(measurement);
    return measurement;
}

Measurement measure_direct_mixed_frame_churn(std::size_t iterations) {
    constexpr std::array<std::size_t, 6> sizes{{128, 256, 512, 1024, 2048, 4096}};
    Measurement measurement{
        .iterations = iterations,
        .fallbackEstimate = iterations,
    };
    MicroTimer timer;
    for (std::size_t i = 0; i < iterations; ++i) {
        void* frame = ::operator new(sizes[i % sizes.size()], std::nothrow);
        if (frame == nullptr) {
            measurement.iterations = i;
            break;
        }
        ::operator delete(frame);
    }
    timer.finish(measurement);
    return measurement;
}

Measurement measure_over_aligned_churn(std::size_t iterations,
                                    std::size_t alignment) {
    Measurement measurement{
        .iterations = iterations,
        .fallbackEstimate = iterations,
    };
    MicroTimer timer;
    for (std::size_t i = 0; i < iterations; ++i) {
        void* frame = allocate_frame_storage(256, alignment);
        if (frame == nullptr) {
            measurement.iterations = i;
            break;
        }
        release_frame_storage(frame, 256, alignment);
    }
    timer.finish(measurement);
    return measurement;
}

Measurement measure_live_burst(std::size_t size,
                             std::size_t frameCount) {
    std::vector<void*> frames(frameCount, nullptr);
    Measurement measurement{
        .iterations = frameCount,
        .fallbackEstimate = frameCount,
    };
    MicroTimer timer;
    for (void*& frame : frames) {
        frame = allocate_frame_storage(size, kAlignment);
        if (frame == nullptr) {
            measurement.iterations =
                static_cast<std::size_t>(&frame - frames.data());
            break;
        }
    }
    for (void* frame : frames) {
        release_frame_storage(frame, size, kAlignment);
    }
    timer.finish(measurement);
    return measurement;
}

Measurement measure_concurrent_frame_churn(std::size_t iterationsPerWorker,
                                         std::size_t workerCount) {
    constexpr std::array<std::size_t, 4> sizes{{128, 256, 512, 2048}};
    std::atomic<std::size_t> completed{0};
    std::vector<std::thread> workers;
    workers.reserve(workerCount);
    MicroTimer timer;
    for (std::size_t worker = 0; worker < workerCount; ++worker) {
        workers.emplace_back([&, worker]() {
            std::size_t localCompleted = 0;
            for (std::size_t i = 0; i < iterationsPerWorker; ++i) {
                const std::size_t size = sizes[(i + worker) % sizes.size()];
                void* frame = allocate_frame_storage(size, kAlignment);
                if (frame == nullptr) {
                    continue;
                }
                release_frame_storage(frame, size, kAlignment);
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
    timer.finish(measurement);
    return measurement;
}

template <std::size_t Payload>
Task<void> payload_task() {
    std::array<std::byte, Payload> payload{};
    co_await std::suspend_always{};
    (void)payload;
}

Task<void> lightweight_task() {
    co_return;
}

Task<int> integer_task() {
    co_return 1;
}

Task<void> suspend_resume_task() {
    co_await std::suspend_always{};
    co_return;
}

template <typename Factory>
Measurement measure_task_create_destroy(Factory&& factory, std::size_t iterations) {
    Measurement measurement{.iterations = iterations};
    MicroTimer timer;
    for (std::size_t i = 0; i < iterations; ++i) {
        auto task = factory();
        if (!task.is_valid()) {
            measurement.iterations = i;
            break;
        }
    }
    timer.finish(measurement);
    return measurement;
}

template <typename Factory>
Measurement measure_task_complete_destroy(Factory&& factory,
                                       std::size_t iterations) {
    Measurement measurement{.iterations = iterations};
    MicroTimer timer;
    for (std::size_t i = 0; i < iterations; ++i) {
        auto task = factory();
        if (!task.is_valid()) {
            measurement.iterations = i;
            break;
        }
        auto* state = galay::kernel::detail::TaskAccess::task_ref(task).state();
        if (state == nullptr || state->m_handle == nullptr) {
            measurement.iterations = i;
            break;
        }
        state->m_handle.resume();
    }
    timer.finish(measurement);
    return measurement;
}

Measurement measure_suspend_resume(std::size_t iterations) {
    Measurement measurement{.iterations = iterations};
    MicroTimer timer;
    for (std::size_t i = 0; i < iterations; ++i) {
        auto task = suspend_resume_task();
        if (!task.is_valid()) {
            measurement.iterations = i;
            break;
        }
        auto* state = galay::kernel::detail::TaskAccess::task_ref(task).state();
        state->m_handle.resume();
        state->m_handle.resume();
    }
    timer.finish(measurement);
    return measurement;
}

Measurement measure_cross_thread_release(std::size_t iterations) {
    std::vector<void*> frames(iterations, nullptr);
    Measurement measurement{
        .iterations = iterations,
        .fallbackEstimate = iterations,
    };
    MicroTimer timer;

    std::thread producer([&frames]() {
        for (void*& frame : frames) {
            frame = allocate_frame_storage(256, kAlignment);
        }
    });
    producer.join();

    std::thread consumer([&frames]() {
        for (void* frame : frames) {
            release_frame_storage(frame, 256, kAlignment);
        }
    });
    consumer.join();

    timer.finish(measurement);
    return measurement;
}

bool measure_retained_memory(std::size_t retainedTasks) {
    std::vector<Task<void>> tasks;
    tasks.reserve(retainedTasks);
    for (std::size_t i = 0; i < retainedTasks; ++i) {
        tasks.push_back(payload_task<256>());
        if (!tasks.back().is_valid()) {
            return false;
        }
    }
    std::cout << "retained_memory tasks=" << tasks.size() << '\n';
    tasks.clear();
    std::cout << "retained_memory_after_clear tasks=0\n";
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (!galay::benchmark::initialize_benchmark_environment()) {
        return 1;
    }

    galay::benchmark::MicroOptions options;
    if (!galay::benchmark::parse_micro_options(argc, argv, options)) { return 1; }
    using Measure = Measurement (*)(std::size_t);
    struct Sample { const char* name; Measure measure; };
    const Sample samples[] = {
        {"frame_size_128", [](size_t n) { return measure_frame_churn(128, n); }},
        {"frame_size_4096", [](size_t n) { return measure_frame_churn(4096, n); }},
        {"frame_mixed_recycler", measure_mixed_frame_churn},
        {"task_void_unsubmitted_destroy", [](size_t n) { return measure_task_create_destroy([] { return lightweight_task(); }, n); }},
        {"task_int_unsubmitted_destroy", [](size_t n) { return measure_task_create_destroy([] { return integer_task(); }, n); }},
        {"task_void_complete_destroy", [](size_t n) { return measure_task_complete_destroy([] { return lightweight_task(); }, n); }},
        {"task_int_complete_destroy", [](size_t n) { return measure_task_complete_destroy([] { return integer_task(); }, n); }},
        {"task_suspend_resume", measure_suspend_resume},
    };
    bool found = false;
    for (const auto& sample : samples) {
        if (!options.sample.empty() && options.sample != sample.name) { continue; }
        found = true;
        const size_t warmup = std::max(size_t(1), options.iterations / 10);
        const auto warmed = sample.measure(warmup);
        if (warmed.iterations != warmup || warmed.cpu_ns < 0) { return 1; }
        const auto measured = sample.measure(options.iterations);
        print_measurement(sample.name, measured);
        if (measured.iterations != options.iterations || measured.cpu_ns < 0) { return 1; }
    }
    if (!found) { std::cerr << "B34: unknown --sample\n"; return 1; }
    if (options.diagnostics) {
        // Fixed bounded pressure work, deliberately excluded from throughput runs.
        constexpr size_t n = 20'000;
        const Sample diagnostics[] = {
            {"frame_churn_recycler", [](size_t count) { return measure_frame_churn(256, count); }},
            {"frame_churn_direct", [](size_t count) { return measure_direct_frame_churn(256, count); }},
            {"frame_size_512", [](size_t count) { return measure_frame_churn(512, count); }},
            {"frame_size_2048", [](size_t count) { return measure_frame_churn(2048, count); }},
            {"frame_mixed_direct", measure_direct_mixed_frame_churn},
            {"frame_overaligned_64", [](size_t count) { return measure_over_aligned_churn(count, kAlignment * 4); }},
            {"cross_thread_release", measure_cross_thread_release},
        };
        for (const auto& sample : diagnostics) {
            const auto measured = sample.measure(n);
            print_measurement(sample.name, measured);
            if (measured.iterations != n || measured.cpu_ns < 0) { return 1; }
        }
        const auto burst = measure_live_burst(128, 4096);
        const auto concurrent = measure_concurrent_frame_churn(50'000, 4);
        print_measurement("frame_live_burst_128", burst);
        print_measurement("frame_concurrent_recycler", concurrent);
        if (burst.iterations != 4096 || concurrent.iterations != 200'000 || !measure_retained_memory(5'000)) { return 1; }
    }
    return 0;
}
