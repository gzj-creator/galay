/**
 * @file t178_runtime_executor_split.cc
 * @brief 验证 Runtime 根任务按 IO / CPU 执行器显式提交。
 */

#include <galay/cpp/galay-kernel/core/runtime.h>

#include <concepts>
#include <cstdlib>
#include <iostream>
#include <thread>

using namespace galay::kernel;

namespace {

Task<int> valueTask(int value)
{
    co_return value;
}

Task<std::thread::id> executionThread()
{
    co_return std::this_thread::get_id();
}

template <typename R>
concept HasRuntimeSpawnIO = requires(R runtime, Task<int> task) {
    { runtime.spawnIO(std::move(task)) } -> std::same_as<std::expected<JoinHandle<int>, RuntimeError>>;
};

template <typename R>
concept HasRuntimeSpawnCpu = requires(R runtime, Task<int> task) {
    { runtime.spawnCpu(std::move(task)) } -> std::same_as<std::expected<JoinHandle<int>, RuntimeError>>;
};

template <typename R>
concept HasRuntimeBlockOnIO = requires(R runtime, Task<int> task) {
    { runtime.blockOnIO(std::move(task)) } -> std::same_as<std::expected<int, RuntimeError>>;
};

template <typename R>
concept HasRuntimeBlockOnCpu = requires(R runtime, Task<int> task) {
    { runtime.blockOnCpu(std::move(task)) } -> std::same_as<std::expected<int, RuntimeError>>;
};

template <typename R>
concept HasHandleSpawnIO = requires(R handle, Task<int> task) {
    { handle.spawnIO(std::move(task)) } -> std::same_as<std::expected<JoinHandle<int>, RuntimeError>>;
};

template <typename R>
concept HasHandleSpawnCpu = requires(R handle, Task<int> task) {
    { handle.spawnCpu(std::move(task)) } -> std::same_as<std::expected<JoinHandle<int>, RuntimeError>>;
};

template <typename R>
concept HasRuntimeSpawn = requires(R runtime, Task<int> task) {
    runtime.spawn(std::move(task));
};

template <typename R>
concept HasHandleSpawn = requires(R handle, Task<int> task) {
    handle.spawn(std::move(task));
};

static_assert(HasRuntimeSpawnIO<Runtime>);
static_assert(HasRuntimeSpawnCpu<Runtime>);
static_assert(HasRuntimeBlockOnIO<Runtime>);
static_assert(HasRuntimeBlockOnCpu<Runtime>);
static_assert(HasHandleSpawnIO<RuntimeHandle>);
static_assert(HasHandleSpawnCpu<RuntimeHandle>);
static_assert(!HasRuntimeSpawn<Runtime>);
static_assert(!HasHandleSpawn<RuntimeHandle>);

void require(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

void requireValue(std::expected<JoinHandle<int>, RuntimeError>& result, int expected, const char* message)
{
    require(result.has_value(), message);
    auto value = result->join();
    require(value.has_value() && *value == expected, message);
}

} // namespace

int main()
{
    Runtime ioRuntime = RuntimeBuilder().ioSchedulerCount(1).parallelSchedulerCount(0).build();
    auto ioTask = ioRuntime.spawnIO(valueTask(11));
    requireValue(ioTask, 11, "spawnIO should submit when an IO scheduler is available");
    auto ioCpuTask = ioRuntime.spawnCpu(valueTask(12));
    require(!ioCpuTask.has_value() && ioCpuTask.error().code() == RuntimeErrorCode::kNoSchedulerAvailable,
            "spawnCpu should reject a runtime without a parallel scheduler");

    Runtime cpuRuntime = RuntimeBuilder().ioSchedulerCount(0).parallelSchedulerCount(1).build();
    auto cpuTask = cpuRuntime.spawnCpu(valueTask(21));
    requireValue(cpuTask, 21, "spawnCpu should submit when a parallel scheduler is available");
    auto cpuIoTask = cpuRuntime.spawnIO(valueTask(22));
    require(!cpuIoTask.has_value() && cpuIoTask.error().code() == RuntimeErrorCode::kNoSchedulerAvailable,
            "spawnIO should reject a runtime without an IO scheduler");

    Runtime mixedRuntime = RuntimeBuilder().ioSchedulerCount(1).parallelSchedulerCount(1).build();
    auto ioBlock = mixedRuntime.blockOnIO(valueTask(41));
    require(ioBlock.has_value() && *ioBlock == 41, "blockOnIO should use the IO scheduler");
    auto cpuBlock = mixedRuntime.blockOnCpu(valueTask(42));
    require(cpuBlock.has_value() && *cpuBlock == 42, "blockOnCpu should use the parallel scheduler");
    auto handle = mixedRuntime.handle();
    auto handleIoTask = handle.spawnIO(valueTask(31));
    requireValue(handleIoTask, 31, "RuntimeHandle::spawnIO should submit to the IO executor");
    auto handleCpuTask = handle.spawnCpu(valueTask(32));
    requireValue(handleCpuTask, 32, "RuntimeHandle::spawnCpu should submit to the CPU executor");

    auto ioThreadTask = mixedRuntime.spawnIO(executionThread());
    require(ioThreadTask.has_value(), "spawnIO should return an execution thread");
    auto ioThread = ioThreadTask->join();
    require(ioThread.has_value() && *ioThread == mixedRuntime.getIOScheduler(0)->threadId(),
            "spawnIO should bind the task to an IO scheduler");

    auto cpuThreadTask = mixedRuntime.spawnCpu(executionThread());
    require(cpuThreadTask.has_value(), "spawnCpu should return an execution thread");
    auto cpuThread = cpuThreadTask->join();
    require(cpuThread.has_value() && *cpuThread == mixedRuntime.getParallelScheduler(0)->threadId(),
            "spawnCpu should bind the task to a parallel scheduler");

    mixedRuntime.stop();
    cpuRuntime.stop();
    ioRuntime.stop();
    return 0;
}
