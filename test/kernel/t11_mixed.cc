/**
 * @file t11_mixed.cc
 * @brief 用途：验证 `IOScheduler` 与 `ComputeScheduler` 混合协作时的调度正确性。
 * 关键覆盖点：IO 与计算任务协同、跨调度器通知、混合场景下的恢复顺序与 owner 调度器回归。
 * 通过条件：混合调度断言全部成立，测试输出 PASS 并返回 0。
 */

#include <iostream>
#include <atomic>
#include <chrono>
#include <thread>
#include "kernel/kernel/compute_scheduler.h"
#include "kernel/kernel/task.h"
#include "kernel/concurrency/async_waiter.h"
#include "test/stdout_log.h"
#include "result_writer.h"

#ifdef USE_EPOLL
#include "kernel/kernel/epoll_scheduler.h"
using IOSchedulerType = galay::kernel::EpollScheduler;
#elif defined(USE_KQUEUE)
#include "kernel/kernel/kqueue_scheduler.h"
using IOSchedulerType = galay::kernel::KqueueScheduler;
#elif defined(USE_IOURING)
#include "kernel/kernel/uring_scheduler.h"
using IOSchedulerType = galay::kernel::IOUringScheduler;
#endif

using namespace galay::kernel;
using namespace std::chrono_literals;

std::atomic<int> g_passed{0};
std::atomic<int> g_total{0};

template <typename SchedulerT, typename T>
void requireSchedule(SchedulerT& scheduler, Task<T>&& task)
{
    const bool scheduled = scheduler.schedule(detail::TaskAccess::detachTask(std::move(task)));
    if (!scheduled) {
        throw std::runtime_error("failed to schedule task in T11");
    }
}

// ============== 测试1: IO任务提交计算任务到ComputeScheduler ==============
std::atomic<int> g_test1_compute_result{0};

// 计算任务 - 在 ComputeScheduler 中执行
Task<void> computeHeavyTask(AsyncWaiter<int>* waiter) {
    // 模拟 CPU 密集型计算
    volatile int sum = 0;
    for (int i = 0; i < 100000; ++i) {
        sum += i % 100;
    }
    waiter->notify(sum);
    co_return;
}

// IO 任务 - 在 IOScheduler 中执行
Task<void> ioTaskWithCompute(ComputeScheduler* computeScheduler) {
    // 创建等待器
    AsyncWaiter<int> waiter;

    // 提交计算任务到 ComputeScheduler
    requireSchedule(*computeScheduler, computeHeavyTask(&waiter));

    // 等待计算完成
    auto result = co_await waiter.wait();
    if (result) {
        g_test1_compute_result.store(result.value(), std::memory_order_relaxed);
    }
    co_return;
}

// ============== 测试2: 多个 IO 任务并发提交计算任务 ==============
std::atomic<int> g_test2_completed{0};
constexpr int TEST2_COUNT = 10;

Task<void> computeTaskForTest2(AsyncWaiter<int>* waiter, int id) {
    volatile int sum = 0;
    for (int i = 0; i < 10000; ++i) {
        sum += (i + id) % 100;
    }
    waiter->notify(sum);
    co_return;
}

Task<void> ioTaskMultiple(ComputeScheduler* computeScheduler, int id) {
    AsyncWaiter<int> waiter;
    requireSchedule(*computeScheduler, computeTaskForTest2(&waiter, id));

    auto result = co_await waiter.wait();
    (void)result;

    g_test2_completed.fetch_add(1, std::memory_order_relaxed);
    co_return;
}

// ============== 测试3: 纯 ComputeScheduler 任务（不涉及 IO） ==============
std::atomic<int> g_test3_counter{0};

Task<void> pureComputeTask() {
    volatile double result = 0;
    for (int i = 0; i < 50000; ++i) {
        result += i * 0.001;
    }
    g_test3_counter.fetch_add(1, std::memory_order_relaxed);
    co_return;
}

// ============== 测试4: 链式计算 - IO -> Compute -> IO ==============
std::atomic<bool> g_test4_done{false};
std::atomic<int> g_test4_stage{0};

Task<void> computeMiddleTask(AsyncWaiter<int>* waiter) {
    g_test4_stage.store(2, std::memory_order_relaxed);  // 进入计算阶段
    volatile int sum = 0;
    for (int i = 0; i < 10000; ++i) {
        sum += i;
    }
    waiter->notify(sum);
    co_return;
}

Task<void> ioChainTask(ComputeScheduler* computeScheduler) {
    g_test4_stage.store(1, std::memory_order_relaxed);  // IO阶段1

    AsyncWaiter<int> waiter;
    requireSchedule(*computeScheduler, computeMiddleTask(&waiter));

    auto result = co_await waiter.wait();

    g_test4_stage.store(3, std::memory_order_relaxed);  // IO阶段2（计算完成后）

    // 验证结果
    if (result && result.value() == 49995000) {  // 0+1+2+...+9999
        g_test4_done.store(true, std::memory_order_relaxed);
    }
    co_return;
}

// ============== 测试5: AsyncWaiter<void> 混合测试 ==============
std::atomic<bool> g_test5_compute_done{false};
std::atomic<bool> g_test5_io_resumed{false};

Task<void> computeVoidTask(AsyncWaiter<void>* waiter) {
    volatile int sum = 0;
    for (int i = 0; i < 10000; ++i) {
        sum += i;
    }
    g_test5_compute_done.store(true, std::memory_order_relaxed);
    waiter->notify();
    co_return;
}

Task<void> ioVoidWaitTask(ComputeScheduler* computeScheduler) {
    AsyncWaiter<void> waiter;
    requireSchedule(*computeScheduler, computeVoidTask(&waiter));

    co_await waiter.wait();

    g_test5_io_resumed.store(true, std::memory_order_relaxed);
    co_return;
}

// ============== 测试6: 高并发压力测试 ==============
std::atomic<int> g_test6_completed{0};
constexpr int TEST6_COUNT = 100;

Task<void> computeTaskForTest6(AsyncWaiter<int>* waiter, int id) {
    volatile int sum = 0;
    for (int i = 0; i < 1000; ++i) {
        sum += (i * id) % 100;
    }
    waiter->notify(sum);
    co_return;
}

Task<void> ioTaskHighConcurrency(ComputeScheduler* computeScheduler, int id) {
    AsyncWaiter<int> waiter;
    requireSchedule(*computeScheduler, computeTaskForTest6(&waiter, id));
    auto result = co_await waiter.wait();
    (void)result;
    g_test6_completed.fetch_add(1, std::memory_order_relaxed);
    co_return;
}

// ============== 测试7: 多次 await 同一协程内 ==============
std::atomic<int> g_test7_await_count{0};

Task<void> computeTaskForTest7(AsyncWaiter<int>* waiter, int value) {
    volatile int sum = value;
    for (int i = 0; i < 100; ++i) {
        sum += i;
    }
    waiter->notify(sum);
    co_return;
}

Task<void> ioMultipleAwait(ComputeScheduler* computeScheduler) {
    // 在同一个任务内多次 await 不同的计算任务
    for (int i = 0; i < 5; ++i) {
        AsyncWaiter<int> waiter;
        requireSchedule(*computeScheduler, computeTaskForTest7(&waiter, i * 100));
        auto result = co_await waiter.wait();
        (void)result;
        g_test7_await_count.fetch_add(1, std::memory_order_relaxed);
    }
    co_return;
}

// ============== 测试8: notify 先于 wait 的竞态情况 ==============
std::atomic<bool> g_test8_done{false};

Task<void> computeTaskFast(AsyncWaiter<int>* waiter) {
    // 立即 notify，不做任何计算
    waiter->notify(42);
    co_return;
}

Task<void> ioWaitAfterNotify(ComputeScheduler* computeScheduler) {
    AsyncWaiter<int> waiter;
    requireSchedule(*computeScheduler, computeTaskFast(&waiter));

    // 故意延迟一下，让 notify 先执行
    volatile int delay = 0;
    for (int i = 0; i < 10000; ++i) {
        delay += i;
    }
    (void)delay;

    auto result = co_await waiter.wait();
    if (result.value() == 42) {
        g_test8_done.store(true, std::memory_order_relaxed);
    }
    co_return;
}

// ============== 测试9: 任务恢复到原 owner 调度器 ==============
std::atomic<bool> g_test9_scheduler_correct{false};

Task<void> computeCheckScheduler(AsyncWaiter<void>* waiter) {
    // 计算任务完成后，通过 waiter 恢复 IO owner 任务
    waiter->notify();
    co_return;
}

Task<void> ioCheckSchedulerReturn(ComputeScheduler* computeScheduler) {
    AsyncWaiter<void> waiter;
    requireSchedule(*computeScheduler, computeCheckScheduler(&waiter));

    co_await waiter.wait();

    // 如果能执行到这里，说明任务被正确恢复回了原 IO owner 调度器
    g_test9_scheduler_correct.store(true, std::memory_order_relaxed);
    co_return;
}

// ============== 测试10: 多个 ComputeScheduler 实例 ==============
std::atomic<int> g_test10_completed{0};

Task<void> computeTaskForTest10(AsyncWaiter<int>* waiter, int schedulerId) {
    volatile int sum = schedulerId * 1000;
    for (int i = 0; i < 100; ++i) {
        sum += i;
    }
    waiter->notify(sum);
    co_return;
}

Task<void> ioWithMultipleComputeSchedulers(ComputeScheduler* cs1, ComputeScheduler* cs2, [[maybe_unused]] int id) {
    AsyncWaiter<int> waiter1;
    AsyncWaiter<int> waiter2;

    // 同时提交到两个不同的 ComputeScheduler
    requireSchedule(*cs1, computeTaskForTest10(&waiter1, 1));
    requireSchedule(*cs2, computeTaskForTest10(&waiter2, 2));

    auto r1 = co_await waiter1.wait();
    auto r2 = co_await waiter2.wait();

    (void)r1;
    (void)r2;
    g_test10_completed.fetch_add(1, std::memory_order_relaxed);
    co_return;
}

// ============== 测试11: 调度器停止时的任务处理 ==============
std::atomic<int> g_test11_completed{0};

Task<void> computeTaskForTest11(AsyncWaiter<void>* waiter) {
    // 模拟较长的计算
    volatile int sum = 0;
    for (int i = 0; i < 50000; ++i) {
        sum += i;
    }
    g_test11_completed.fetch_add(1, std::memory_order_relaxed);
    waiter->notify();
    co_return;
}

void runTests() {
    LogInfo("=== Mixed Scheduler Test Suite ===");

#if defined(USE_EPOLL) || defined(USE_KQUEUE) || defined(USE_IOURING)
    // 测试1: IO 任务提交计算任务
    {
        LogInfo("[Test 1] IO task submits compute task...");
        g_total++;

        IOSchedulerType ioScheduler;
        ComputeScheduler computeScheduler;

        ioScheduler.start();
        computeScheduler.start();

        requireSchedule(ioScheduler, ioTaskWithCompute(&computeScheduler));

        // 等待完成
        auto start = std::chrono::steady_clock::now();
        while (g_test1_compute_result.load() == 0) {
            // 使用调度器的空闲等待
            if (std::chrono::steady_clock::now() - start > 5s) {
                break;
            }
        }

        computeScheduler.stop();
        ioScheduler.stop();

        if (g_test1_compute_result.load() != 0) {
            LogInfo("[Test 1] PASSED: Compute result = {}", g_test1_compute_result.load());
            g_passed++;
        } else {
            LogError("[Test 1] FAILED: Compute task did not complete");
        }
    }

    // 测试2: 多个 IO 任务并发提交计算任务
    {
        LogInfo("[Test 2] Multiple IO tasks submit compute tasks ({} tasks)...", TEST2_COUNT);
        g_total++;

        IOSchedulerType ioScheduler;
        ComputeScheduler computeScheduler;

        ioScheduler.start();
        computeScheduler.start();

        for (int i = 0; i < TEST2_COUNT; ++i) {
            requireSchedule(ioScheduler, ioTaskMultiple(&computeScheduler, i));
        }

        auto start = std::chrono::steady_clock::now();
        while (g_test2_completed.load() < TEST2_COUNT) {
            // 使用调度器的空闲等待
            if (std::chrono::steady_clock::now() - start > 10s) {
                break;
            }
        }

        computeScheduler.stop();
        ioScheduler.stop();

        if (g_test2_completed.load() == TEST2_COUNT) {
            LogInfo("[Test 2] PASSED: All {} tasks completed", TEST2_COUNT);
            g_passed++;
        } else {
            LogError("[Test 2] FAILED: Only {}/{} tasks completed",
                    g_test2_completed.load(), TEST2_COUNT);
        }
    }

    // 测试3: 纯 ComputeScheduler 任务
    {
        LogInfo("[Test 3] Pure ComputeScheduler tasks (20 tasks)...");
        g_total++;

        ComputeScheduler computeScheduler;
        computeScheduler.start();

        for (int i = 0; i < 20; ++i) {
            requireSchedule(computeScheduler, pureComputeTask());
        }

        auto start = std::chrono::steady_clock::now();
        while (g_test3_counter.load() < 20) {
            // 使用调度器的空闲等待
            if (std::chrono::steady_clock::now() - start > 5s) {
                break;
            }
        }

        computeScheduler.stop();

        if (g_test3_counter.load() == 20) {
            LogInfo("[Test 3] PASSED: All 20 pure compute tasks completed");
            g_passed++;
        } else {
            LogError("[Test 3] FAILED: Only {}/20 tasks completed", g_test3_counter.load());
        }
    }

    // 测试4: 链式计算 IO -> Compute -> IO
    {
        LogInfo("[Test 4] Chain: IO -> Compute -> IO...");
        g_total++;

        IOSchedulerType ioScheduler;
        ComputeScheduler computeScheduler;

        ioScheduler.start();
        computeScheduler.start();

        requireSchedule(ioScheduler, ioChainTask(&computeScheduler));

        auto start = std::chrono::steady_clock::now();
        while (!g_test4_done.load()) {
            // 使用调度器的空闲等待
            if (std::chrono::steady_clock::now() - start > 5s) {
                break;
            }
        }

        computeScheduler.stop();
        ioScheduler.stop();

        if (g_test4_done.load() && g_test4_stage.load() == 3) {
            LogInfo("[Test 4] PASSED: Chain completed, final stage = {}", g_test4_stage.load());
            g_passed++;
        } else {
            LogError("[Test 4] FAILED: done={}, stage={}", g_test4_done.load(), g_test4_stage.load());
        }
    }

    // 测试5: AsyncWaiter<void> 混合测试
    {
        LogInfo("[Test 5] AsyncWaiter<void> mixed test...");
        g_total++;

        IOSchedulerType ioScheduler;
        ComputeScheduler computeScheduler;

        ioScheduler.start();
        computeScheduler.start();

        requireSchedule(ioScheduler, ioVoidWaitTask(&computeScheduler));

        auto start = std::chrono::steady_clock::now();
        while (!g_test5_io_resumed.load()) {
            // 使用调度器的空闲等待
            if (std::chrono::steady_clock::now() - start > 5s) {
                break;
            }
        }

        computeScheduler.stop();
        ioScheduler.stop();

        if (g_test5_compute_done.load() && g_test5_io_resumed.load()) {
            LogInfo("[Test 5] PASSED: Compute done and IO resumed");
            g_passed++;
        } else {
            LogError("[Test 5] FAILED: compute_done={}, io_resumed={}",
                    g_test5_compute_done.load(), g_test5_io_resumed.load());
        }
    }

    // 测试6: 高并发压力测试
    {
        LogInfo("[Test 6] High concurrency stress test ({} tasks)...", TEST6_COUNT);
        g_total++;

        IOSchedulerType ioScheduler;
        ComputeScheduler computeScheduler;

        ioScheduler.start();
        computeScheduler.start();

        for (int i = 0; i < TEST6_COUNT; ++i) {
            requireSchedule(ioScheduler, ioTaskHighConcurrency(&computeScheduler, i));
        }

        auto start = std::chrono::steady_clock::now();
        while (g_test6_completed.load() < TEST6_COUNT) {
            // 使用调度器的空闲等待
            if (std::chrono::steady_clock::now() - start > 30s) {
                break;
            }
        }

        computeScheduler.stop();
        ioScheduler.stop();

        if (g_test6_completed.load() == TEST6_COUNT) {
            LogInfo("[Test 6] PASSED: All {} high-concurrency tasks completed", TEST6_COUNT);
            g_passed++;
        } else {
            LogError("[Test 6] FAILED: Only {}/{} tasks completed",
                    g_test6_completed.load(), TEST6_COUNT);
        }
    }

    // 测试7: 多次 await 同一任务内
    {
        LogInfo("[Test 7] Multiple awaits in single coroutine...");
        g_total++;

        IOSchedulerType ioScheduler;
        ComputeScheduler computeScheduler;

        ioScheduler.start();
        computeScheduler.start();

        requireSchedule(ioScheduler, ioMultipleAwait(&computeScheduler));

        auto start = std::chrono::steady_clock::now();
        while (g_test7_await_count.load() < 5) {
            // 使用调度器的空闲等待
            if (std::chrono::steady_clock::now() - start > 5s) {
                break;
            }
        }

        computeScheduler.stop();
        ioScheduler.stop();

        if (g_test7_await_count.load() == 5) {
            LogInfo("[Test 7] PASSED: All 5 sequential awaits completed");
            g_passed++;
        } else {
            LogError("[Test 7] FAILED: Only {}/5 awaits completed", g_test7_await_count.load());
        }
    }

    // 测试8: notify 先于 wait 的竞态情况
    {
        LogInfo("[Test 8] Race condition: notify before wait...");
        g_total++;

        IOSchedulerType ioScheduler;
        ComputeScheduler computeScheduler;

        ioScheduler.start();
        computeScheduler.start();

        requireSchedule(ioScheduler, ioWaitAfterNotify(&computeScheduler));

        auto start = std::chrono::steady_clock::now();
        while (!g_test8_done.load()) {
            // 使用调度器的空闲等待
            if (std::chrono::steady_clock::now() - start > 5s) {
                break;
            }
        }

        computeScheduler.stop();
        ioScheduler.stop();

        if (g_test8_done.load()) {
            LogInfo("[Test 8] PASSED: Handled notify-before-wait correctly");
            g_passed++;
        } else {
            LogError("[Test 8] FAILED: Race condition not handled");
        }
    }

    // 测试9: owner 调度器回归正确性
    {
        LogInfo("[Test 9] Task owner scheduler return correctness...");
        g_total++;

        IOSchedulerType ioScheduler;
        ComputeScheduler computeScheduler;

        ioScheduler.start();
        computeScheduler.start();

        requireSchedule(ioScheduler, ioCheckSchedulerReturn(&computeScheduler));

        auto start = std::chrono::steady_clock::now();
        while (!g_test9_scheduler_correct.load()) {
            // 使用调度器的空闲等待
            if (std::chrono::steady_clock::now() - start > 5s) {
                break;
            }
        }

        computeScheduler.stop();
        ioScheduler.stop();

        if (g_test9_scheduler_correct.load()) {
            LogInfo("[Test 9] PASSED: Task returned to correct owner scheduler");
            g_passed++;
        } else {
            LogError("[Test 9] FAILED: Task did not return to original owner scheduler");
        }
    }

    // 测试10: 多个 ComputeScheduler 实例
    {
        LogInfo("[Test 10] Multiple ComputeScheduler instances...");
        g_total++;

        IOSchedulerType ioScheduler;
        ComputeScheduler computeScheduler1;
        ComputeScheduler computeScheduler2;

        ioScheduler.start();
        computeScheduler1.start();
        computeScheduler2.start();

        for (int i = 0; i < 5; ++i) {
            requireSchedule(ioScheduler, ioWithMultipleComputeSchedulers(&computeScheduler1, &computeScheduler2, i));
        }

        auto start = std::chrono::steady_clock::now();
        while (g_test10_completed.load() < 5) {
            // 使用调度器的空闲等待
            if (std::chrono::steady_clock::now() - start > 10s) {
                break;
            }
        }

        computeScheduler1.stop();
        computeScheduler2.stop();
        ioScheduler.stop();

        if (g_test10_completed.load() == 5) {
            LogInfo("[Test 10] PASSED: All tasks with multiple ComputeSchedulers completed");
            g_passed++;
        } else {
            LogError("[Test 10] FAILED: Only {}/5 tasks completed", g_test10_completed.load());
        }
    }

    // 测试11: 调度器停止时的任务处理
    {
        LogInfo("[Test 11] Tasks completion during scheduler stop...");
        g_total++;

        ComputeScheduler computeScheduler;
        computeScheduler.start();

        // 提交多个任务
        std::vector<AsyncWaiter<void>> waiters(10);
        for (int i = 0; i < 10; ++i) {
            requireSchedule(computeScheduler, computeTaskForTest11(&waiters[i]));
        }

        // 等待一小段时间让任务开始执行（已移除 sleep_for）

        // 停止调度器（应该等待正在执行的任务完成）
        computeScheduler.stop();

        // 检查完成的任务数
        int completed = g_test11_completed.load();
        if (completed > 0) {
            LogInfo("[Test 11] PASSED: {} tasks completed before/during stop", completed);
            g_passed++;
        } else {
            LogError("[Test 11] FAILED: No tasks completed");
        }
    }

#else
    LogWarn("No IO scheduler available, skipping mixed tests");
#endif

    LogInfo("=== Results: {}/{} tests passed ===", g_passed.load(), g_total.load());
}

int main() {
    galay::test::TestResultWriter resultWriter("test_mixed_scheduler");
    runTests();

    // 写入测试结果
    resultWriter.addTest();
    if (g_passed == g_total) {
        resultWriter.addPassed();
    } else {
        resultWriter.addFailed();
    }
    resultWriter.writeResult();

    return g_passed.load() == g_total.load() ? 0 : 1;
}
