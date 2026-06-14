/**
 * @file t20_spawn.cc
 * @brief 用途：验证基础 `spawn` 提交后任务能够被调度并顺利执行完成。
 * 关键覆盖点：任务提交、调度器接管执行、简单完成通知与收尾流程。
 * 通过条件：被提交任务确实执行完成，测试返回 0。
 */

#include "kernel/kernel/runtime.h"
#include <atomic>
#include <cassert>
#include <chrono>
#include <iostream>
#include <thread>

using namespace galay::kernel;

namespace {

std::atomic<int> g_finishedTasks{0};

Task<int> computeTask(int id, int yields)
{
    assert(RuntimeHandle::tryCurrent().has_value());

    for (int i = 0; i < yields; ++i) {
        co_yield true;
    }

    g_finishedTasks.fetch_add(1, std::memory_order_acq_rel);
    co_return id * 10;
}

Task<void> spawnDetachedTasks()
{
    auto runtime_handle = RuntimeHandle::current();
    assert(runtime_handle.has_value());

    auto handle = runtime_handle->spawn(computeTask(2, 2));
    auto detached = runtime_handle->spawn(computeTask(3, 3));
    assert(handle.has_value());
    assert(detached.has_value());
    (void)handle;
    (void)detached;
    co_return;
}

Task<void> waitForFinishedTasks(int expected)
{
    for (int i = 0; i < 4096 && g_finishedTasks.load(std::memory_order_acquire) < expected; ++i) {
        co_yield true;
    }

    assert(g_finishedTasks.load(std::memory_order_acquire) == expected);
    co_return;
}

Task<int> rootValue()
{
    co_return 42;
}

Task<void> spawnBlockingFromHandle()
{
    auto runtime_handle = RuntimeHandle::current();
    assert(runtime_handle.has_value());

    auto blocking = runtime_handle->spawnBlocking([]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        return 7;
    });
    assert(blocking.has_value());

    auto value = blocking->join();

    assert(value.has_value());
    assert(*value == 7);
    co_return;
}

} // namespace

int main()
{
    Runtime runtime = RuntimeBuilder()
        .ioSchedulerCount(0)
        .computeSchedulerCount(1)
        .build();

    const auto value = runtime.blockOn(rootValue());
    assert(value.has_value());
    assert(*value == 42);

    auto joined = runtime.spawn(computeTask(1, 1));
    assert(joined.has_value());
    auto joined_value = joined->join();
    assert(joined_value.has_value());
    assert(*joined_value == 10);

    auto detached = runtime.blockOn(spawnDetachedTasks());
    assert(detached.has_value());
    auto finished = runtime.blockOn(waitForFinishedTasks(3));
    assert(finished.has_value());
    auto blocking = runtime.blockOn(spawnBlockingFromHandle());
    assert(blocking.has_value());

    std::cout << "T20-RuntimeTaskApiDemo PASS\n";
    return 0;
}
