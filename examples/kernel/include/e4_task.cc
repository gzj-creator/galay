/**
 * @file e4_task.cc
 * @brief 用途：用头文件方式演示 `Runtime + Task + blockOn/spawn` 的基础用法。
 * 关键覆盖点：根任务 `blockOn`、任务 `spawn`、`JoinHandle` 与 `RuntimeHandle`。
 * 通过条件：关键演示路径全部执行完成并返回 0。
 */

#include "galay-kernel/core/task.h"
#include "galay-kernel/core/runtime.h"
#include "test/stdout_log.h"
#include <cassert>
#include <atomic>
#include <chrono>
#include <thread>

using namespace galay::kernel;

namespace {

std::atomic<int> g_detachedFinished{0};

Task<int> sumTask(int id, int limit)
{
    LogInfo("Task {} started", id);

    int sum = 0;
    for (int i = 0; i < limit; ++i) {
        sum += i;
    }

    LogInfo("Task {} completed, sum = {}", id, sum);
    co_return sum;
}

Task<void> detachedTask(int id)
{
    LogInfo("Detached task {} started", id);
    co_yield true;
    g_detachedFinished.fetch_add(1, std::memory_order_acq_rel);
    LogInfo("Detached task {} completed", id);
    co_return;
}

Task<void> spawnFromCurrentRuntime()
{
    LogInfo("Spawning detached tasks through RuntimeHandle::current()");
    auto runtimeHandle = RuntimeHandle::current();
    assert(runtimeHandle.has_value());
    auto first = runtimeHandle->spawn(detachedTask(1));
    auto second = runtimeHandle->spawn(detachedTask(2));
    assert(first.has_value());
    assert(second.has_value());
    co_return;
}

Task<void> waitForDetachedTasks()
{
    for (int i = 0; i < 1024 && g_detachedFinished.load(std::memory_order_acquire) < 2; ++i) {
        co_yield true;
    }

    LogInfo("Detached tasks finished: {}", g_detachedFinished.load(std::memory_order_acquire));
    co_return;
}

Task<void> spawnBlockingDemo()
{
    auto runtimeHandle = RuntimeHandle::current();
    assert(runtimeHandle.has_value());

    auto blocking = runtimeHandle->spawnBlocking([]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        return 7;
    });
    assert(blocking.has_value());
    auto waitResult = blocking->wait();
    assert(waitResult.has_value());
    auto value = blocking->join();
    assert(value.has_value());

    LogInfo("spawnBlocking returned {}", *value);
    co_return;
}

} // namespace

int main()
{
    LogInfo("=== Runtime Task API Basic Example ===");

    Runtime runtime = RuntimeBuilder()
        .ioSchedulerCount(0)
        .computeSchedulerCount(1)
        .build();

    LogInfo("\n--- Example 1: blockOn(Task<int>) ---");
    auto rootValue = runtime.blockOn(sumTask(1, 1000));
    assert(rootValue.has_value());
    LogInfo("blockOn returned {}", *rootValue);

    LogInfo("\n--- Example 2: spawn(Task<int>) -> JoinHandle<int> ---");
    auto handle = runtime.spawn(sumTask(2, 2000));
    assert(handle.has_value());
    auto handleWait = handle->wait();
    assert(handleWait.has_value());
    auto handleValue = handle->join();
    assert(handleValue.has_value());
    LogInfo("spawn().join() returned {}", *handleValue);

    LogInfo("\n--- Example 3: RuntimeHandle::current().spawn(...) ---");
    auto spawnResult = runtime.blockOn(spawnFromCurrentRuntime());
    assert(spawnResult.has_value());
    auto waitResult = runtime.blockOn(waitForDetachedTasks());
    assert(waitResult.has_value());

    LogInfo("\n--- Example 4: RuntimeHandle::spawnBlocking(...) ---");
    auto blockingResult = runtime.blockOn(spawnBlockingDemo());
    assert(blockingResult.has_value());

    LogInfo("\n=== Example Completed ===");
    return 0;
}
