/**
 * @file e4_task.cc
 * @brief 用途：用模块导入方式演示 `Runtime + Task + blockOn/spawn` 的基础用法。
 * 关键覆盖点：`galay.kernel` 模块导入、根任务 `blockOn`、任务派生与等待。
 * 通过条件：关键演示路径全部执行完成并返回 0。
 */

import galay.kernel;

#include <cassert>
#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>

using namespace galay::kernel;

namespace {

std::atomic<int> g_detachedFinished{0};

Task<int> sumTask(int id, int limit)
{
    std::cout << "task " << id << " started\n";

    int sum = 0;
    for (int i = 0; i < limit; ++i) {
        sum += i;
    }

    std::cout << "task " << id << " completed, sum=" << sum << "\n";
    co_return sum;
}

Task<void> detachedTask(int id)
{
    std::cout << "detached task " << id << " started\n";
    co_yield true;
    g_detachedFinished.fetch_add(1, std::memory_order_acq_rel);
    std::cout << "detached task " << id << " completed\n";
    co_return;
}

Task<void> spawnFromCurrentRuntime()
{
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

    std::cout << "detached tasks finished: " << g_detachedFinished.load(std::memory_order_acquire) << "\n";
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

    std::cout << "spawnBlocking returned " << *value << "\n";
    co_return;
}

} // namespace

int main()
{
    Runtime runtime = RuntimeBuilder()
        .ioSchedulerCount(0)
        .computeSchedulerCount(1)
        .build();

    auto rootValue = runtime.blockOn(sumTask(1, 1000));
    assert(rootValue.has_value());
    std::cout << "blockOn returned " << *rootValue << "\n";

    auto handle = runtime.spawn(sumTask(2, 2000));
    assert(handle.has_value());
    auto handleWait = handle->wait();
    assert(handleWait.has_value());
    auto handleValue = handle->join();
    assert(handleValue.has_value());
    std::cout << "spawn().join() returned " << *handleValue << "\n";

    auto spawnResult = runtime.blockOn(spawnFromCurrentRuntime());
    assert(spawnResult.has_value());
    auto waitResult = runtime.blockOn(waitForDetachedTasks());
    assert(waitResult.has_value());
    auto blockingResult = runtime.blockOn(spawnBlockingDemo());
    assert(blockingResult.has_value());

    return 0;
}
