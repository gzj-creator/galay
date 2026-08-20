/**
 * @file b30_runtime_executor_split.cc
 * @brief 测量 Runtime 显式 IO / CPU 根任务入口的提交与完成吞吐。
 */

#include <galay/cpp/galay-kernel/core/runtime.h>

#include <chrono>
#include <iostream>
#include <vector>

using namespace galay::kernel;

namespace {

Task<int> benchmarkTask()
{
    co_return 1;
}

template <typename Submit>
bool runBatch(Runtime& runtime, Submit&& submit, size_t count)
{
    std::vector<JoinHandle<int>> handles;
    handles.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        auto handle = submit(benchmarkTask());
        if (!handle.has_value()) {
            return false;
        }
        handles.push_back(std::move(*handle));
    }
    for (auto& handle : handles) {
        auto result = handle.join();
        if (!result.has_value() || *result != 1) {
            return false;
        }
    }
    (void)runtime;
    return true;
}

} // namespace

int main()
{
    constexpr size_t taskCount = 10000;
    Runtime runtime = RuntimeBuilder().ioSchedulerCount(1).computeSchedulerCount(1).build();

    const auto start = std::chrono::steady_clock::now();
    const bool ioOk = runBatch(runtime, [&runtime](Task<int> task) {
        return runtime.spawnIO(std::move(task));
    }, taskCount);
    const bool cpuOk = runBatch(runtime, [&runtime](Task<int> task) {
        return runtime.spawnCpu(std::move(task));
    }, taskCount);
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - start);

    runtime.stop();
    if (!ioOk || !cpuOk) {
        return 1;
    }

    std::cout << "runtime_executor_split tasks=" << (taskCount * 2)
              << " elapsed_us=" << elapsed.count() << '\n';
    return 0;
}
