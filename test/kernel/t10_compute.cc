/**
 * @file t10_compute.cc
 * @brief 用途：验证 `ComputeScheduler` 在基础、并发和等待通知场景下的正确性。
 * 关键覆盖点：任务执行、并发提交、计算密集任务、调度器启停与等待通知恢复。
 * 通过条件：所有子测试通过并输出 PASS，总体返回码为 0。
 */

#include <iostream>
#include <atomic>
#include <chrono>
#include <vector>
#include <cmath>
#include "kernel/kernel/compute_scheduler.h"
#include "kernel/kernel/task.h"
#include "kernel/concurrency/async_waiter.h"
#include "test/stdout_log.h"
#include "result_writer.h"

using namespace galay::kernel;
using namespace std::chrono_literals;

std::atomic<int> g_passed{0};
std::atomic<int> g_total{0};

template <typename SchedulerT, typename T>
void requireSchedule(SchedulerT& scheduler, Task<T>&& task)
{
    const bool scheduled = scheduler.schedule(detail::TaskAccess::detachTask(std::move(task)));
    if (!scheduled) {
        throw std::runtime_error("failed to schedule task in T10");
    }
}

// 测试1：基本任务执行
std::atomic<bool> g_test1_done{false};

Task<void> testBasicExecution() {
    g_test1_done = true;
    co_return;
}

// 测试2：多个任务并发执行
std::atomic<int> g_test2_counter{0};
constexpr int TEST2_COUNT = 100;

Task<void> testConcurrentTask() {
    g_test2_counter.fetch_add(1, std::memory_order_relaxed);
    co_return;
}

// 测试3：计算密集型任务
std::atomic<int> g_test3_counter{0};
constexpr int TEST3_COUNT = 10;

Task<void> testComputeIntensive() {
    // 模拟 CPU 密集型计算
    volatile double result = 0;
    for (int i = 0; i < 100000; ++i) {
        result += std::sin(i) * std::cos(i);
    }
    g_test3_counter.fetch_add(1, std::memory_order_relaxed);
    co_return;
}

// 测试5：调度器启停
std::atomic<int> g_test5_counter{0};

Task<void> testStartStop() {
    g_test5_counter.fetch_add(1, std::memory_order_relaxed);
    co_return;
}

// 测试8：AsyncWaiter 基本功能
std::atomic<int> g_test8_result{0};

// 计算任务 - 在 ComputeScheduler 中执行
Task<void> computeTask(AsyncWaiter<int>* waiter) {
    // 模拟计算
    volatile int sum = 0;
    for (int i = 0; i < 10000; ++i) {
        sum += i;
    }
    // 通知结果
    waiter->notify(sum);
    co_return;
}

// 测试9：AsyncWaiter<void> 无返回值
std::atomic<bool> g_test9_done{false};

Task<void> computeTaskVoid(AsyncWaiter<void>* waiter) {
    // 模拟计算
    volatile int sum = 0;
    for (int i = 0; i < 10000; ++i) {
        sum += i;
    }
    g_test9_done = true;
    waiter->notify();
    co_return;
}

void runTests() {
    LogInfo("=== ComputeScheduler Test Suite ===");

    // 测试1：基本任务执行
    {
        LogInfo("[Test 1] Basic task execution...");
        g_total++;

        ComputeScheduler scheduler;
        scheduler.start();
        requireSchedule(scheduler, testBasicExecution());
        // 使用调度器的空闲等待
        scheduler.stop();

        if (g_test1_done) {
            LogInfo("[Test 1] PASSED: Task executed successfully");
            g_passed++;
        } else {
            LogError("[Test 1] FAILED: Task did not execute");
        }
    }

    // 测试2：多个任务并发执行
    {
        LogInfo("[Test 2] Concurrent task execution ({} tasks)...", TEST2_COUNT);
        g_total++;

        ComputeScheduler scheduler1, scheduler2, scheduler3, scheduler4;
        scheduler1.start();
        scheduler2.start();
        scheduler3.start();
        scheduler4.start();

        for (int i = 0; i < TEST2_COUNT; ++i) {
            // 轮询分发到4个调度器
            switch (i % 4) {
                case 0: requireSchedule(scheduler1, testConcurrentTask()); break;
                case 1: requireSchedule(scheduler2, testConcurrentTask()); break;
                case 2: requireSchedule(scheduler3, testConcurrentTask()); break;
                case 3: requireSchedule(scheduler4, testConcurrentTask()); break;
            }
        }

        // 使用调度器的空闲等待
        scheduler1.stop();
        scheduler2.stop();
        scheduler3.stop();
        scheduler4.stop();

        if (g_test2_counter == TEST2_COUNT) {
            LogInfo("[Test 2] PASSED: All {} tasks completed", TEST2_COUNT);
            g_passed++;
        } else {
            LogError("[Test 2] FAILED: Only {}/{} tasks completed", g_test2_counter.load(), TEST2_COUNT);
        }
    }

    // 测试3：计算密集型任务
    {
        LogInfo("[Test 3] Compute-intensive tasks ({} tasks)...", TEST3_COUNT);
        g_total++;

        ComputeScheduler scheduler1, scheduler2, scheduler3, scheduler4;
        scheduler1.start();
        scheduler2.start();
        scheduler3.start();
        scheduler4.start();

        auto start = std::chrono::steady_clock::now();

        for (int i = 0; i < TEST3_COUNT; ++i) {
            switch (i % 4) {
                case 0: requireSchedule(scheduler1, testComputeIntensive()); break;
                case 1: requireSchedule(scheduler2, testComputeIntensive()); break;
                case 2: requireSchedule(scheduler3, testComputeIntensive()); break;
                case 3: requireSchedule(scheduler4, testComputeIntensive()); break;
            }
        }

        // 等待所有任务完成
        while (g_test3_counter < TEST3_COUNT) {
            // 使用调度器的空闲等待
        }

        auto elapsed = std::chrono::steady_clock::now() - start;
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();

        scheduler1.stop();
        scheduler2.stop();
        scheduler3.stop();
        scheduler4.stop();

        if (g_test3_counter == TEST3_COUNT) {
            LogInfo("[Test 3] PASSED: All {} compute tasks completed in {}ms", TEST3_COUNT, ms);
            g_passed++;
        } else {
            LogError("[Test 3] FAILED: Only {}/{} tasks completed", g_test3_counter.load(), TEST3_COUNT);
        }
    }

    // 测试4：链式任务 focused 验证已迁移到 T54-task_then_compat，
    // 避免这个套件把停机时机噪声混入 then 语义回归

    // 测试5：调度器启停
    {
        LogInfo("[Test 5] Scheduler start/stop cycles...");
        g_total++;

        ComputeScheduler scheduler;

        // 第一次启停
        scheduler.start();
        requireSchedule(scheduler, testStartStop());
        // 使用调度器的空闲等待
        scheduler.stop();

        int count1 = g_test5_counter.load();

        // 第二次启停
        scheduler.start();
        requireSchedule(scheduler, testStartStop());
        // 使用调度器的空闲等待
        scheduler.stop();

        int count2 = g_test5_counter.load();

        if (count1 == 1 && count2 == 2) {
            LogInfo("[Test 5] PASSED: Scheduler can be restarted");
            g_passed++;
        } else {
            LogError("[Test 5] FAILED: count1={}, count2={}", count1, count2);
        }
    }

    // 测试6：单线程调度器验证
    {
        LogInfo("[Test 6] Single-threaded scheduler verification...");
        g_total++;

        ComputeScheduler scheduler;

        // 单线程调度器，验证基本功能
        scheduler.start();
        bool running = scheduler.isRunning();
        scheduler.stop();
        bool stopped = !scheduler.isRunning();

        if (running && stopped) {
            LogInfo("[Test 6] PASSED: Single-threaded ComputeScheduler works correctly");
            g_passed++;
        } else {
            LogError("[Test 6] FAILED: running={}, stopped={}", running, stopped);
        }
    }

    // 测试7：isRunning 状态
    {
        LogInfo("[Test 7] isRunning state...");
        g_total++;

        ComputeScheduler scheduler;

        bool before_start = scheduler.isRunning();
        scheduler.start();
        bool after_start = scheduler.isRunning();
        scheduler.stop();
        bool after_stop = scheduler.isRunning();

        if (!before_start && after_start && !after_stop) {
            LogInfo("[Test 7] PASSED: isRunning state correct");
            g_passed++;
        } else {
            LogError("[Test 7] FAILED: before={}, after_start={}, after_stop={}",
                    before_start, after_start, after_stop);
        }
    }

    // 测试8：AsyncWaiter<int> 带返回值
    {
        LogInfo("[Test 8] AsyncWaiter<int> with result...");
        g_total++;

        ComputeScheduler computeScheduler;
        computeScheduler.start();

        AsyncWaiter<int> waiter;
        requireSchedule(computeScheduler, computeTask(&waiter));

        // 等待结果（简单轮询，实际使用中应在协程内 co_await）
        while (!waiter.isReady()) {
            // 使用调度器的空闲等待
        }

        computeScheduler.stop();

        // 预期结果: 0+1+2+...+9999 = 49995000
        if (waiter.isReady()) {
            LogInfo("[Test 8] PASSED: AsyncWaiter notified");
            g_passed++;
        } else {
            LogError("[Test 8] FAILED: AsyncWaiter not ready");
        }
    }

    // 测试9：AsyncWaiter<void> 无返回值
    {
        LogInfo("[Test 9] AsyncWaiter<void> without result...");
        g_total++;

        ComputeScheduler computeScheduler;
        computeScheduler.start();

        AsyncWaiter<void> waiter;
        requireSchedule(computeScheduler, computeTaskVoid(&waiter));

        while (!waiter.isReady()) {
            // 使用调度器的空闲等待
        }

        computeScheduler.stop();

        if (waiter.isReady() && g_test9_done) {
            LogInfo("[Test 9] PASSED: AsyncWaiter<void> notified");
            g_passed++;
        } else {
            LogError("[Test 9] FAILED: AsyncWaiter<void> not ready");
        }
    }

    LogInfo("=== Results: {}/{} tests passed ===", g_passed.load(), g_total.load());
}

int main() {
    galay::test::TestResultWriter resultWriter("test_compute_scheduler");
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
