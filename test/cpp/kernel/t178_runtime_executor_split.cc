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

Task<int> value_task(int value)
{
    co_return value;
}

Task<std::thread::id> execution_thread()
{
    co_return std::this_thread::get_id();
}

template <typename R>
concept HasRuntimeSpawnIO = requires(R runtime, Task<int> task) {
    { runtime.spawn_io(std::move(task)) } -> std::same_as<std::expected<JoinHandle<int>, RuntimeError>>;
};

template <typename R>
concept HasRuntimeSpawnCpu = requires(R runtime, Task<int> task) {
    { runtime.spawn_cpu(std::move(task)) } -> std::same_as<std::expected<JoinHandle<int>, RuntimeError>>;
};

template <typename R>
concept HasRuntimeBlockOnIO = requires(R runtime, Task<int> task) {
    { runtime.block_on_io(std::move(task)) } -> std::same_as<std::expected<int, RuntimeError>>;
};

template <typename R>
concept HasRuntimeBlockOnCpu = requires(R runtime, Task<int> task) {
    { runtime.block_on_cpu(std::move(task)) } -> std::same_as<std::expected<int, RuntimeError>>;
};

template <typename R>
concept HasHandleSpawnIO = requires(R handle, Task<int> task) {
    { handle.spawn_io(std::move(task)) } -> std::same_as<std::expected<JoinHandle<int>, RuntimeError>>;
};

template <typename R>
concept HasHandleSpawnCpu = requires(R handle, Task<int> task) {
    { handle.spawn_cpu(std::move(task)) } -> std::same_as<std::expected<JoinHandle<int>, RuntimeError>>;
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

void require_value(std::expected<JoinHandle<int>, RuntimeError>& result, int expected, const char* message)
{
    require(result.has_value(), message);
    auto value = result->join();
    require(value.has_value() && *value == expected, message);
}

} // namespace

int main()
{
    Runtime ioRuntime = RuntimeBuilder().io_scheduler_count(1).parallel_scheduler_count(0).build();
    auto ioTask = ioRuntime.spawn_io(value_task(11));
    require_value(ioTask, 11, "spawnIO should submit when an IO scheduler is available");
    auto ioCpuTask = ioRuntime.spawn_cpu(value_task(12));
    require(!ioCpuTask.has_value() && ioCpuTask.error().code() == RuntimeErrorCode::kNoSchedulerAvailable,
            "spawnCpu should reject a runtime without a parallel scheduler");

    Runtime cpuRuntime = RuntimeBuilder().io_scheduler_count(0).parallel_scheduler_count(1).build();
    auto cpuTask = cpuRuntime.spawn_cpu(value_task(21));
    require_value(cpuTask, 21, "spawnCpu should submit when a parallel scheduler is available");
    auto cpuIoTask = cpuRuntime.spawn_io(value_task(22));
    require(!cpuIoTask.has_value() && cpuIoTask.error().code() == RuntimeErrorCode::kNoSchedulerAvailable,
            "spawnIO should reject a runtime without an IO scheduler");

    Runtime mixedRuntime = RuntimeBuilder().io_scheduler_count(1).parallel_scheduler_count(1).build();
    auto ioBlock = mixedRuntime.block_on_io(value_task(41));
    require(ioBlock.has_value() && *ioBlock == 41, "blockOnIO should use the IO scheduler");
    auto cpuBlock = mixedRuntime.block_on_cpu(value_task(42));
    require(cpuBlock.has_value() && *cpuBlock == 42, "blockOnCpu should use the parallel scheduler");
    auto handle = mixedRuntime.handle();
    auto handleIoTask = handle.spawn_io(value_task(31));
    require_value(handleIoTask, 31, "RuntimeHandle::spawnIO should submit to the IO executor");
    auto handleCpuTask = handle.spawn_cpu(value_task(32));
    require_value(handleCpuTask, 32, "RuntimeHandle::spawnCpu should submit to the CPU executor");

    auto ioThreadTask = mixedRuntime.spawn_io(execution_thread());
    require(ioThreadTask.has_value(), "spawnIO should return an execution thread");
    auto ioThread = ioThreadTask->join();
    require(ioThread.has_value() && *ioThread == mixedRuntime.get_io_scheduler(0)->thread_id(),
            "spawnIO should bind the task to an IO scheduler");

    auto cpuThreadTask = mixedRuntime.spawn_cpu(execution_thread());
    require(cpuThreadTask.has_value(), "spawnCpu should return an execution thread");
    auto cpuThread = cpuThreadTask->join();
    require(cpuThread.has_value() && *cpuThread == mixedRuntime.get_parallel_scheduler(0)->thread_id(),
            "spawnCpu should bind the task to a parallel scheduler");

    mixedRuntime.stop();
    cpuRuntime.stop();
    ioRuntime.stop();
    return 0;
}
