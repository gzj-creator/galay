/**
 * @file t52_taskapi.cc
 * @brief 用途：验证 `Runtime`、`Task` 与 `JoinHandle` 的公开 API 形态保持完整。
 * 关键覆盖点：编译期概念检查、公开成员存在性、关键运行时 API 语义。
 * 通过条件：编译期与运行期检查全部通过，测试返回 0。
 */

#include "kernel/kernel/runtime.h"

#include <concepts>
#include <cstdlib>
#include <expected>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

using namespace galay::kernel;

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << std::endl;
        std::exit(1);
    }
}

Task<int> simpleTask(int value) {
    co_return value;
}

Task<void> verifyCurrentRuntimeHandle() {
    auto current = RuntimeHandle::tryCurrent();
    require(current.has_value(), "RuntimeHandle::tryCurrent should succeed inside runtime context");

    auto current_result = RuntimeHandle::current();
    require(current_result.has_value(), "RuntimeHandle::current should succeed inside runtime context");

    auto nested = current_result->spawn(simpleTask(19));
    require(nested.has_value(), "RuntimeHandle::spawn should succeed inside runtime context");
    auto nested_value = nested->join();
    require(nested_value.has_value(), "RuntimeHandle::current nested join should succeed");
    require(*nested_value == 19, "RuntimeHandle::current should return a working handle");
    co_return;
}

template <typename T>
concept HasJoinHandleResult = requires(JoinHandle<T> handle) {
    handle.result();
};

template <typename T>
concept HasTaskRef = requires(Task<T> task) {
    task.taskRef();
};

template <typename T>
concept HasBelongSchedulerGetter = requires(Task<T> task) {
    task.belongScheduler();
};

template <typename T>
concept HasBelongSchedulerSetter = requires(Task<T> task, Scheduler* scheduler) {
    task.belongScheduler(scheduler);
};

template <typename T>
concept HasThreadId = requires(Task<T> task) {
    task.threadId();
};

template <typename T>
concept HasAsCoroutine = requires(Task<T> task) {
    task.asCoroutine();
};

template <typename T>
concept HasJoinHandleWait = requires(JoinHandle<T> handle) {
    { handle.wait() } -> std::same_as<std::expected<void, detail::TaskResultError>>;
};

template <typename T>
concept HasJoinHandleJoinExpected = requires(JoinHandle<T> handle) {
    { handle.join() } -> std::same_as<std::expected<T, detail::TaskResultError>>;
};

template <typename = void>
concept HasRuntimeHandleCurrent = requires {
    { RuntimeHandle::current() } -> std::same_as<std::expected<RuntimeHandle, RuntimeError>>;
};

template <typename = void>
concept HasRuntimeHandleTryCurrent = requires {
    { RuntimeHandle::tryCurrent() } -> std::same_as<std::optional<RuntimeHandle>>;
};

struct BlockingCallable {
    int operator()() const { return 23; }
};

template <typename R>
concept HasRuntimeSpawnBlocking = requires(R runtime) {
    { runtime.spawnBlocking(BlockingCallable{}) } -> std::same_as<std::expected<JoinHandle<int>, RuntimeError>>;
};

template <typename R>
concept HasRuntimeBlockOnExpected = requires(R runtime, Task<int> task) {
    { runtime.blockOn(std::move(task)) } -> std::same_as<std::expected<int, RuntimeError>>;
};

template <typename R>
concept HasRuntimeSpawnExpected = requires(R runtime, Task<int> task) {
    { runtime.spawn(std::move(task)) } -> std::same_as<std::expected<JoinHandle<int>, RuntimeError>>;
};

template <typename ErrorT>
concept HasErrorMessage = requires(const ErrorT& error) {
    { error.message() } -> std::same_as<std::string_view>;
};

template <typename C>
concept HasTaskThenLvalue = requires(C& left, C right) {
    { left.then(std::move(right)) } -> std::same_as<C&>;
};

template <typename C>
concept HasTaskThenRvalue = requires(C left, C right) {
    { std::move(left).then(std::move(right)) } -> std::same_as<C&&>;
};

template <typename C>
concept HasTaskAwaitOperator = requires(C task) {
    std::move(task).operator co_await();
};

}  // namespace

static_assert(!HasJoinHandleResult<int>);
static_assert(!HasTaskRef<int>);
static_assert(!HasBelongSchedulerGetter<int>);
static_assert(!HasBelongSchedulerSetter<int>);
static_assert(!HasThreadId<int>);
static_assert(!HasAsCoroutine<int>);
static_assert(HasJoinHandleWait<int>);
static_assert(HasJoinHandleJoinExpected<int>);
static_assert(HasRuntimeHandleCurrent<>);
static_assert(HasRuntimeHandleTryCurrent<>);
static_assert(HasRuntimeSpawnBlocking<Runtime>);
static_assert(HasRuntimeBlockOnExpected<Runtime>);
static_assert(HasRuntimeSpawnExpected<Runtime>);
static_assert(HasErrorMessage<RuntimeError>);
static_assert(HasErrorMessage<detail::TaskResultError>);
static_assert(HasErrorMessage<BlockingExecutorError>);
static_assert(HasTaskThenLvalue<Task<void>>);
static_assert(HasTaskThenRvalue<Task<void>>);
static_assert(HasTaskAwaitOperator<Task<int>>);
static_assert(HasTaskAwaitOperator<Task<void>>);

int main() {
    Runtime runtime;

    require(!RuntimeHandle::tryCurrent().has_value(), "RuntimeHandle::tryCurrent should be empty outside runtime context");
    require(!RuntimeHandle::current().has_value(), "RuntimeHandle::current should fail outside runtime context");
    require(RuntimeHandle::current().error().code() == RuntimeErrorCode::kNoCurrentRuntime,
            "RuntimeHandle::current should report missing runtime context");

    auto blockResult = runtime.blockOn(simpleTask(7));
    require(blockResult.has_value(), "Runtime::blockOn should succeed");
    require(*blockResult == 7, "Runtime::blockOn should return task result");

    auto joinHandle = runtime.spawn(simpleTask(11));
    require(joinHandle.has_value(), "Runtime::spawn should succeed");
    require(joinHandle->wait().has_value(), "Runtime::spawn wait should succeed");
    auto joinedValue = joinHandle->join();
    require(joinedValue.has_value(), "Runtime::spawn join should succeed");
    require(*joinedValue == 11, "Runtime::spawn should return joinable handle");

    auto handle = runtime.handle();
    auto handleJoin = handle.spawn(simpleTask(13));
    require(handleJoin.has_value(), "RuntimeHandle::spawn should succeed");
    require(handleJoin->wait().has_value(), "RuntimeHandle::spawn wait should succeed");
    auto handleJoinValue = handleJoin->join();
    require(handleJoinValue.has_value(), "RuntimeHandle::spawn join should succeed");
    require(*handleJoinValue == 13, "RuntimeHandle::spawn should submit task");

    auto runtimeBlockingJoin = runtime.spawnBlocking([]() { return 17; });
    require(runtimeBlockingJoin.has_value(), "Runtime::spawnBlocking should succeed");
    require(runtimeBlockingJoin->wait().has_value(), "Runtime::spawnBlocking wait should succeed");
    auto runtimeBlockingValue = runtimeBlockingJoin->join();
    require(runtimeBlockingValue.has_value(), "Runtime::spawnBlocking join should succeed");
    require(*runtimeBlockingValue == 17, "Runtime::spawnBlocking should return task result");

    auto blockingJoin = handle.spawnBlocking([]() { return 17; });
    require(blockingJoin.has_value(), "RuntimeHandle::spawnBlocking should succeed");
    require(blockingJoin->wait().has_value(), "RuntimeHandle::spawnBlocking wait should succeed");
    auto blockingValue = blockingJoin->join();
    require(blockingValue.has_value(), "RuntimeHandle::spawnBlocking join should succeed");
    require(*blockingValue == 17, "RuntimeHandle::spawnBlocking should return task result");

    auto handleBlock = runtime.blockOn(verifyCurrentRuntimeHandle());
    require(handleBlock.has_value(), "Runtime::blockOn should return expected<void> success");

    RuntimeHandle emptyHandle;
    auto emptyHandleSpawn = emptyHandle.spawn(simpleTask(21));
    require(!emptyHandleSpawn.has_value(), "empty RuntimeHandle::spawn should fail without throwing");
    require(emptyHandleSpawn.error().code() == RuntimeErrorCode::kInvalidHandle,
            "empty RuntimeHandle::spawn should report invalid handle");
    require(!emptyHandleSpawn.error().message().empty(),
            "RuntimeError::message should describe invalid handle");
    require(emptyHandleSpawn.error().message().find("RuntimeError::") == std::string::npos,
            "RuntimeError::message should expose a reason instead of an enum label");

    Runtime noSchedulerRuntime = RuntimeBuilder()
        .ioSchedulerCount(0)
        .computeSchedulerCount(0)
        .build();
    auto noSchedulerResult = noSchedulerRuntime.blockOn(simpleTask(23));
    require(!noSchedulerResult.has_value(), "Runtime::blockOn should fail when no scheduler is available");
    require(noSchedulerResult.error().code() == RuntimeErrorCode::kNoSchedulerAvailable,
            "Runtime::blockOn should report missing scheduler");
    require(!noSchedulerResult.error().message().empty(),
            "RuntimeError::message should describe missing scheduler");
    require(noSchedulerResult.error().message().find("RuntimeError::") == std::string::npos,
            "RuntimeError::message should expose a reason instead of an enum label");

    detail::TaskResultError taskError(detail::TaskResultErrorCode::kAlreadyConsumed);
    require(!taskError.message().empty(), "TaskResultError::message should describe the error");
    require(taskError.message().find("TaskResultError::") == std::string::npos,
            "TaskResultError::message should expose a reason instead of an enum label");

    BlockingExecutorError blockingError(BlockingExecutorErrorCode::kStopping);
    require(!blockingError.message().empty(), "BlockingExecutorError::message should describe the error");
    require(blockingError.message().find("BlockingExecutorError::") == std::string::npos,
            "BlockingExecutorError::message should expose a reason instead of an enum label");

    return 0;
}
