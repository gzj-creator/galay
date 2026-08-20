/**
 * @file b30_runtime_executor_split.cc
 * @brief 压测 Runtime 显式 IO / CPU 根任务入口的提交到完成吞吐。
 *
 * 计时前启动 Runtime 并完成预热；正式样本使用 detached Task<void> 和完成闩锁，
 * 避免逐个 join 结果带来的额外噪声，分别报告 IO / CPU 入口的中位数吞吐。
 */

#include "benchmark/cpp/common/benchmark_sync.h"
#include <galay/cpp/galay-kernel/core/runtime.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <string_view>
#include <vector>

using namespace galay::kernel;

namespace {

constexpr std::size_t kWarmupTasks = 10'000;
constexpr std::size_t kMeasuredTasks = 100'000;
constexpr std::size_t kSamples = 5;

struct BatchResult {
    bool ok = false;
    std::size_t completed = 0;
    std::size_t submit_failures = 0;
    std::int64_t elapsed_us = 0;
};

Task<void> pressureTask(galay::benchmark::CompletionLatch* completion,
                        std::atomic<std::size_t>* completed)
{
    completed->fetch_add(1, std::memory_order_relaxed);
    completion->arrive();
    co_return;
}

template <typename Submit>
BatchResult runBatch(Submit&& submit, std::size_t count)
{
    galay::benchmark::CompletionLatch completion(count);
    std::atomic<std::size_t> completed{0};
    std::size_t submit_failures = 0;

    const auto started = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < count; ++i) {
        auto handle = submit(pressureTask(&completion, &completed));
        if (!handle.has_value()) {
            ++submit_failures;
            completion.arrive();
        }
        // Dropping the handle intentionally exercises the detached root-task path.
    }
    completion.wait();

    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - started);
    const auto completed_count = completed.load(std::memory_order_relaxed);
    return BatchResult{
        submit_failures == 0 && completed_count == count,
        completed_count,
        submit_failures,
        elapsed.count()};
}

template <typename Submit>
bool runMode(std::string_view mode, Submit&& submit)
{
    const auto warmup = runBatch(submit, kWarmupTasks);
    if (!warmup.ok) {
        std::cerr << "runtime_executor_pressure warmup_failed mode=" << mode
                  << " completed=" << warmup.completed
                  << " submit_failures=" << warmup.submit_failures << '\n';
        return false;
    }

    std::vector<std::int64_t> samples;
    samples.reserve(kSamples);
    for (std::size_t sample = 0; sample < kSamples; ++sample) {
        const auto result = runBatch(submit, kMeasuredTasks);
        if (!result.ok || result.elapsed_us <= 0) {
            std::cerr << "runtime_executor_pressure sample_failed mode=" << mode
                      << " sample=" << sample
                      << " completed=" << result.completed
                      << " submit_failures=" << result.submit_failures << '\n';
            return false;
        }
        samples.push_back(result.elapsed_us);
    }

    std::sort(samples.begin(), samples.end());
    const auto median_us = samples[samples.size() / 2];
    const double throughput =
        static_cast<double>(kMeasuredTasks) * 1'000'000.0 / static_cast<double>(median_us);

    std::cout << "runtime_executor_pressure mode=" << mode
              << " tasks=" << kMeasuredTasks
              << " samples=" << kSamples
              << " median_us=" << median_us
              << " min_us=" << samples.front()
              << " max_us=" << samples.back()
              << " throughput_tasks_s=" << std::fixed << std::setprecision(0) << throughput
              << '\n';
    return true;
}

} // namespace

int main()
{
    Runtime runtime = RuntimeBuilder().ioSchedulerCount(1).computeSchedulerCount(1).build();
    const auto started = runtime.start();
    if (!started.has_value()) {
        std::cerr << "runtime_executor_pressure runtime_start_failed: "
                  << started.error().message() << '\n';
        return 1;
    }

    const bool io_ok = runMode("io", [&runtime](Task<void> task) {
        return runtime.spawnIO(std::move(task));
    });
    const bool cpu_ok = runMode("cpu", [&runtime](Task<void> task) {
        return runtime.spawnCpu(std::move(task));
    });

    runtime.stop();
    return io_ok && cpu_ok ? 0 : 1;
}
