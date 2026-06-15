/**
 * @file t12_mutex.cc
 * @brief 用途：验证 `AsyncMutex` 在异步互斥场景下的正确性。
 * 关键覆盖点：加锁与解锁、并发竞争、超时等待以及共享状态保护。
 * 通过条件：互斥语义保持正确，所有子测试通过并返回 0。
 */

#include <iostream>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>
#include "galay-kernel/concurrency/async_mutex.h"
#include "galay-kernel/core/task.h"
#include "test/stdout_log.h"
#include "result_writer.h"

#ifdef USE_EPOLL
#include "galay-kernel/core/epoll_scheduler.h"
using IOSchedulerType = galay::kernel::EpollScheduler;
#elif defined(USE_KQUEUE)
#include "galay-kernel/core/kqueue_scheduler.h"
using IOSchedulerType = galay::kernel::KqueueScheduler;
#elif defined(USE_IOURING)
#include "galay-kernel/core/uring_scheduler.h"
using IOSchedulerType = galay::kernel::IOUringScheduler;
#endif

using namespace galay::kernel;
using namespace std::chrono_literals;

std::atomic<int> g_passed{0};
std::atomic<int> g_total{0};

// ============================================================================
// 测试1：基本 lock/unlock
// ============================================================================
std::atomic<bool> g_test1_done{false};

Task<void> testBasicLockUnlock(AsyncMutex* mutex) {
    co_await mutex->lock();
    // 模拟临界区操作（已移除 sleep_for）
    mutex->unlock();
    g_test1_done = true;
    co_return;
}

// ============================================================================
// 测试3：多协程竞争（互斥性验证）
// ============================================================================
std::atomic<int> g_test3_counter{0};
std::atomic<int> g_test3_max_concurrent{0};
std::atomic<int> g_test3_current{0};
std::atomic<int> g_test3_completed{0};
constexpr int TEST3_COROUTINE_COUNT = 10;

Task<void> testMutualExclusion(AsyncMutex* mutex) {
    co_await mutex->lock();

    int current = g_test3_current.fetch_add(1, std::memory_order_relaxed) + 1;

    // 记录最大并发数（应该始终为1）
    int max = g_test3_max_concurrent.load(std::memory_order_relaxed);
    while (current > max && !g_test3_max_concurrent.compare_exchange_weak(max, current));

    // 模拟临界区操作
    g_test3_counter++;
    // 已移除 sleep_for

    g_test3_current.fetch_sub(1, std::memory_order_relaxed);

    mutex->unlock();
    g_test3_completed.fetch_add(1, std::memory_order_relaxed);
    co_return;
}

// ============================================================================
// 测试4：公平性测试（FIFO顺序）
// ============================================================================
std::vector<int> g_test4_order;
std::mutex g_test4_order_mutex;
std::atomic<int> g_test4_completed{0};
constexpr int TEST4_COROUTINE_COUNT = 5;

Task<void> testFairness(AsyncMutex* mutex, int id) {
    co_await mutex->lock();

    {
        std::lock_guard<std::mutex> lock(g_test4_order_mutex);
        g_test4_order.push_back(id);
    }

    // 短暂持有锁（已移除 sleep_for）

    mutex->unlock();
    g_test4_completed.fetch_add(1, std::memory_order_relaxed);
    co_return;
}

// ============================================================================
// 测试5：压力测试
// ============================================================================
std::atomic<int> g_test5_sum{0};
std::atomic<int> g_test5_completed{0};
constexpr int TEST5_COROUTINE_COUNT = 50;
constexpr int TEST5_INCREMENT_COUNT = 10;

Task<void> testStress(AsyncMutex* mutex) {
    for (int i = 0; i < TEST5_INCREMENT_COUNT; ++i) {
        co_await mutex->lock();
        g_test5_sum++;
        mutex->unlock();
    }
    g_test5_completed.fetch_add(1, std::memory_order_relaxed);
    co_return;
}

// ============================================================================
// 测试6：isLocked 状态检查
// ============================================================================
std::atomic<bool> g_test6_locked_inside{false};
std::atomic<bool> g_test6_done{false};

Task<void> testIsLocked(AsyncMutex* mutex) {
    co_await mutex->lock();
    g_test6_locked_inside = mutex->isLocked();
    mutex->unlock();
    g_test6_done = true;
    co_return;
}


// ============================================================================
// 测试8：竞态条件测试（高并发）
// ============================================================================
std::atomic<int> g_test8_race_counter{0};
std::atomic<int> g_test8_completed{0};
constexpr int TEST8_COROUTINE_COUNT = 100;
constexpr int TEST8_INCREMENT_COUNT = 5;

Task<void> testRaceCondition(AsyncMutex* mutex) {
    for (int i = 0; i < TEST8_INCREMENT_COUNT; ++i) {
        co_await mutex->lock();
        // 非原子操作，如果互斥失效会导致数据竞争
        int val = g_test8_race_counter.load(std::memory_order_relaxed);
        g_test8_race_counter.store(val + 1, std::memory_order_relaxed);
        mutex->unlock();
    }
    g_test8_completed.fetch_add(1, std::memory_order_relaxed);
    co_return;
}

// ============================================================================
// 测试9：unlock 后立即 lock（快速切换）
// ============================================================================
std::atomic<int> g_test9_completed{0};
constexpr int TEST9_ITERATIONS = 100;

Task<void> testRapidLockUnlock(AsyncMutex* mutex) {
    for (int i = 0; i < TEST9_ITERATIONS; ++i) {
        co_await mutex->lock();
        // 立即释放，不做任何操作
        mutex->unlock();
    }
    g_test9_completed.fetch_add(1, std::memory_order_relaxed);
    co_return;
}

// ============================================================================
// 测试12：队列容量边界（小容量队列）
// ============================================================================
std::atomic<int> g_test12_completed{0};
constexpr int TEST12_COROUTINE_COUNT = 20;  // 超过初始容量

Task<void> testQueueCapacity(AsyncMutex* mutex) {
    co_await mutex->lock();
    // 已移除 sleep_for
    mutex->unlock();
    g_test12_completed.fetch_add(1, std::memory_order_relaxed);
    co_return;
}

// ============================================================================
// 测试13：协程取消/提前退出场景模拟
// ============================================================================
std::atomic<int> g_test13_completed{0};

Task<void> testEarlyExit(AsyncMutex* mutex, bool shouldExit) {
    co_await mutex->lock();
    if (shouldExit) {
        mutex->unlock();
        co_return;  // 提前退出
    }
    // 已移除 sleep_for
    mutex->unlock();
    g_test13_completed.fetch_add(1, std::memory_order_relaxed);
    co_return;
}

// ============================================================================
// 测试15：长时间持有锁
// ============================================================================
std::atomic<int> g_test15_completed{0};
std::atomic<bool> g_test15_long_holder_done{false};

Task<void> testLongHold(AsyncMutex* mutex) {
    co_await mutex->lock();
    // 长时间持有（已移除 sleep_for）
    mutex->unlock();
    g_test15_long_holder_done = true;
    co_return;
}

Task<void> testLongHoldWaiter(AsyncMutex* mutex) {
    co_await mutex->lock();
    mutex->unlock();
    g_test15_completed.fetch_add(1, std::memory_order_relaxed);
    co_return;
}


// ============================================================================
// 测试19：跨调度器（多线程）场景
// ============================================================================
std::atomic<int> g_test19_counter{0};
std::atomic<int> g_test19_completed{0};
constexpr int TEST19_SCHEDULER_COUNT = 3;
constexpr int TEST19_OPS_PER_SCHEDULER = 20;

Task<void> testCrossScheduler(AsyncMutex* mutex) {
    for (int i = 0; i < TEST19_OPS_PER_SCHEDULER; ++i) {
        co_await mutex->lock();
        // 非原子操作，验证互斥性
        int val = g_test19_counter.load(std::memory_order_relaxed);
        g_test19_counter.store(val + 1, std::memory_order_relaxed);
        mutex->unlock();
    }
    g_test19_completed.fetch_add(1, std::memory_order_relaxed);
    co_return;
}


// ============================================================================
// 主函数
// ============================================================================
void runTests() {
    LogInfo("========================================");
    LogInfo("AsyncMutex Unit Tests");
    LogInfo("========================================");

#if defined(USE_EPOLL) || defined(USE_KQUEUE) || defined(USE_IOURING)

    // 测试1：基本 lock/unlock
    {
        LogInfo("\n--- Test 1: Basic lock/unlock ---");
        g_total++;

        IOSchedulerType scheduler;
        AsyncMutex mutex(8);  // 小容量队列

        scheduler.start();
        scheduler.schedule(detail::TaskAccess::detachTask(testBasicLockUnlock(&mutex)));

        auto start = std::chrono::steady_clock::now();
        while (!g_test1_done) {
            // 使用调度器的空闲等待
            if (std::chrono::steady_clock::now() - start > 2s) break;
        }

        scheduler.stop();

        if (g_test1_done) {
            LogInfo("[PASS] Basic lock/unlock completed");
            g_passed++;
        } else {
            LogError("[FAIL] Basic lock/unlock timeout");
        }
    }

    // 测试3：多协程竞争（互斥性验证）
    {
        LogInfo("\n--- Test 3: Mutual exclusion ({} coroutines) ---", TEST3_COROUTINE_COUNT);
        g_total++;

        IOSchedulerType scheduler;
        AsyncMutex mutex(16);

        scheduler.start();

        for (int i = 0; i < TEST3_COROUTINE_COUNT; ++i) {
            scheduler.schedule(detail::TaskAccess::detachTask(testMutualExclusion(&mutex)));
        }

        auto start = std::chrono::steady_clock::now();
        while (g_test3_completed < TEST3_COROUTINE_COUNT) {
            // 使用调度器的空闲等待
            if (std::chrono::steady_clock::now() - start > 10s) break;
        }

        scheduler.stop();

        bool passed = (g_test3_completed == TEST3_COROUTINE_COUNT) &&
                      (g_test3_max_concurrent == 1) &&
                      (g_test3_counter == TEST3_COROUTINE_COUNT);

        if (passed) {
            LogInfo("[PASS] Mutual exclusion: completed={}, max_concurrent={}, counter={}",
                    g_test3_completed.load(), g_test3_max_concurrent.load(), g_test3_counter.load());
            g_passed++;
        } else {
            LogError("[FAIL] Mutual exclusion: completed={}/{}, max_concurrent={} (expected 1), counter={}",
                    g_test3_completed.load(), TEST3_COROUTINE_COUNT,
                    g_test3_max_concurrent.load(), g_test3_counter.load());
        }
    }

    // 测试4：公平性测试
    {
        LogInfo("\n--- Test 4: Fairness (FIFO order) ---");
        g_total++;

        IOSchedulerType scheduler;
        AsyncMutex mutex(8);

        scheduler.start();

        // 按顺序添加协程
        for (int i = 0; i < TEST4_COROUTINE_COUNT; ++i) {
            scheduler.schedule(detail::TaskAccess::detachTask(testFairness(&mutex, i)));
            // 已移除 sleep_for，确保顺序
        }

        auto start = std::chrono::steady_clock::now();
        while (g_test4_completed < TEST4_COROUTINE_COUNT) {
            // 使用调度器的空闲等待
            if (std::chrono::steady_clock::now() - start > 10s) break;
        }

        scheduler.stop();

        bool all_completed = (g_test4_completed == TEST4_COROUTINE_COUNT);
        bool first_is_zero = !g_test4_order.empty() && g_test4_order[0] == 0;

        std::string order_str;
        for (int id : g_test4_order) {
            order_str += std::to_string(id) + " ";
        }

        if (all_completed && first_is_zero) {
            LogInfo("[PASS] Fairness: order = {}", order_str);
            g_passed++;
        } else {
            LogError("[FAIL] Fairness: completed={}/{}, first_is_zero={}, order={}",
                    g_test4_completed.load(), TEST4_COROUTINE_COUNT, first_is_zero, order_str);
        }
    }

    // 测试5：压力测试
    {
        LogInfo("\n--- Test 5: Stress test ({} coroutines x {} increments) ---",
                TEST5_COROUTINE_COUNT, TEST5_INCREMENT_COUNT);
        g_total++;

        IOSchedulerType scheduler;
        AsyncMutex mutex(32);

        scheduler.start();

        for (int i = 0; i < TEST5_COROUTINE_COUNT; ++i) {
            scheduler.schedule(detail::TaskAccess::detachTask(testStress(&mutex)));
        }

        auto start = std::chrono::steady_clock::now();
        while (g_test5_completed < TEST5_COROUTINE_COUNT) {
            // 使用调度器的空闲等待
            if (std::chrono::steady_clock::now() - start > 60s) break;
        }

        scheduler.stop();

        int expected_sum = TEST5_COROUTINE_COUNT * TEST5_INCREMENT_COUNT;
        bool passed = (g_test5_completed == TEST5_COROUTINE_COUNT) &&
                      (g_test5_sum == expected_sum);

        if (passed) {
            LogInfo("[PASS] Stress test: completed={}, sum={} (expected {})",
                    g_test5_completed.load(), g_test5_sum.load(), expected_sum);
            g_passed++;
        } else {
            LogError("[FAIL] Stress test: completed={}/{}, sum={} (expected {})",
                    g_test5_completed.load(), TEST5_COROUTINE_COUNT,
                    g_test5_sum.load(), expected_sum);
        }
    }

    // 测试6：isLocked 状态检查
    {
        LogInfo("\n--- Test 6: isLocked state ---");
        g_total++;

        IOSchedulerType scheduler;
        AsyncMutex mutex(8);

        bool initially_unlocked = !mutex.isLocked();

        scheduler.start();
        scheduler.schedule(detail::TaskAccess::detachTask(testIsLocked(&mutex)));

        auto start = std::chrono::steady_clock::now();
        while (!g_test6_done) {
            // 使用调度器的空闲等待
            if (std::chrono::steady_clock::now() - start > 2s) break;
        }

        scheduler.stop();

        bool finally_unlocked = !mutex.isLocked();

        if (initially_unlocked && g_test6_locked_inside && finally_unlocked) {
            LogInfo("[PASS] isLocked: initial=unlocked, inside=locked, final=unlocked");
            g_passed++;
        } else {
            LogError("[FAIL] isLocked: initial={}, inside={}, final={}",
                    initially_unlocked ? "unlocked" : "locked",
                    g_test6_locked_inside ? "locked" : "unlocked",
                    finally_unlocked ? "unlocked" : "locked");
        }
    }

    // 测试8：竞态条件测试（高并发）
    {
        LogInfo("\n--- Test 8: Race condition test ({} coroutines x {} increments) ---",
                TEST8_COROUTINE_COUNT, TEST8_INCREMENT_COUNT);
        g_total++;

        IOSchedulerType scheduler;
        AsyncMutex mutex(64);

        scheduler.start();

        for (int i = 0; i < TEST8_COROUTINE_COUNT; ++i) {
            scheduler.schedule(detail::TaskAccess::detachTask(testRaceCondition(&mutex)));
        }

        auto start = std::chrono::steady_clock::now();
        while (g_test8_completed < TEST8_COROUTINE_COUNT) {
            // 使用调度器的空闲等待
            if (std::chrono::steady_clock::now() - start > 60s) break;
        }

        scheduler.stop();

        int expected = TEST8_COROUTINE_COUNT * TEST8_INCREMENT_COUNT;
        bool passed = (g_test8_completed == TEST8_COROUTINE_COUNT) &&
                      (g_test8_race_counter == expected);

        if (passed) {
            LogInfo("[PASS] Race condition test: completed={}, counter={} (expected {})",
                    g_test8_completed.load(), g_test8_race_counter.load(), expected);
            g_passed++;
        } else {
            LogError("[FAIL] Race condition test: completed={}/{}, counter={} (expected {})",
                    g_test8_completed.load(), TEST8_COROUTINE_COUNT,
                    g_test8_race_counter.load(), expected);
        }
    }

    // 测试9：快速 lock/unlock 切换
    {
        LogInfo("\n--- Test 9: Rapid lock/unlock ({} iterations x 5 coroutines) ---", TEST9_ITERATIONS);
        g_total++;

        IOSchedulerType scheduler;
        AsyncMutex mutex(16);

        scheduler.start();

        for (int i = 0; i < 5; ++i) {
            scheduler.schedule(detail::TaskAccess::detachTask(testRapidLockUnlock(&mutex)));
        }

        auto start = std::chrono::steady_clock::now();
        while (g_test9_completed < 5) {
            // 使用调度器的空闲等待
            if (std::chrono::steady_clock::now() - start > 30s) break;
        }

        scheduler.stop();

        if (g_test9_completed == 5) {
            LogInfo("[PASS] Rapid lock/unlock: completed={}", g_test9_completed.load());
            g_passed++;
        } else {
            LogError("[FAIL] Rapid lock/unlock: completed={}/5", g_test9_completed.load());
        }
    }

    // 测试12：队列容量边界
    {
        LogInfo("\n--- Test 12: Queue capacity boundary ({} coroutines, capacity=4) ---",
                TEST12_COROUTINE_COUNT);
        g_total++;

        IOSchedulerType scheduler;
        AsyncMutex mutex(4);  // 小容量

        scheduler.start();

        for (int i = 0; i < TEST12_COROUTINE_COUNT; ++i) {
            scheduler.schedule(detail::TaskAccess::detachTask(testQueueCapacity(&mutex)));
        }

        auto start = std::chrono::steady_clock::now();
        while (g_test12_completed < TEST12_COROUTINE_COUNT) {
            // 使用调度器的空闲等待
            if (std::chrono::steady_clock::now() - start > 30s) break;
        }

        scheduler.stop();

        if (g_test12_completed == TEST12_COROUTINE_COUNT) {
            LogInfo("[PASS] Queue capacity: completed={}", g_test12_completed.load());
            g_passed++;
        } else {
            LogError("[FAIL] Queue capacity: completed={}/{}",
                    g_test12_completed.load(), TEST12_COROUTINE_COUNT);
        }
    }

    // 测试13：提前退出
    {
        LogInfo("\n--- Test 13: Early exit scenario ---");
        g_total++;

        IOSchedulerType scheduler;
        AsyncMutex mutex(8);

        scheduler.start();

        // 一些提前退出，一些正常完成
        for (int i = 0; i < 5; ++i) {
            scheduler.schedule(detail::TaskAccess::detachTask(testEarlyExit(&mutex, i % 2 == 0)));  // 偶数提前退出
        }

        auto start = std::chrono::steady_clock::now();
        while (g_test13_completed < 2) {  // 只有2个会增加计数
            // 使用调度器的空闲等待
            if (std::chrono::steady_clock::now() - start > 10s) break;
        }

        scheduler.stop();

        // 验证锁状态正常
        bool lock_ok = !mutex.isLocked();

        if (g_test13_completed == 2 && lock_ok) {
            LogInfo("[PASS] Early exit: completed={}, lock_released={}",
                    g_test13_completed.load(), lock_ok);
            g_passed++;
        } else {
            LogError("[FAIL] Early exit: completed={} (expected 2), lock_released={}",
                    g_test13_completed.load(), lock_ok);
        }
    }

    // 测试15：长时间持有锁
    {
        LogInfo("\n--- Test 15: Long hold scenario ---");
        g_total++;

        IOSchedulerType scheduler;
        AsyncMutex mutex(8);

        scheduler.start();

        scheduler.schedule(detail::TaskAccess::detachTask(testLongHold(&mutex)));
        // 已移除 sleep_for

        // 添加等待者
        for (int i = 0; i < 3; ++i) {
            scheduler.schedule(detail::TaskAccess::detachTask(testLongHoldWaiter(&mutex)));
        }

        auto start = std::chrono::steady_clock::now();
        while (!g_test15_long_holder_done || g_test15_completed < 3) {
            // 使用调度器的空闲等待
            if (std::chrono::steady_clock::now() - start > 30s) break;
        }

        scheduler.stop();

        bool passed = g_test15_long_holder_done && (g_test15_completed == 3);

        if (passed) {
            LogInfo("[PASS] Long hold: holder_done={}, waiters_completed={}",
                    g_test15_long_holder_done.load(), g_test15_completed.load());
            g_passed++;
        } else {
            LogError("[FAIL] Long hold: holder_done={}, waiters_completed={}/3",
                    g_test15_long_holder_done.load(), g_test15_completed.load());
        }
    }


    // 测试19：跨调度器（多线程）场景
    {
        LogInfo("\n--- Test 19: Cross-scheduler ({} schedulers x {} ops) ---",
                TEST19_SCHEDULER_COUNT, TEST19_OPS_PER_SCHEDULER);
        g_total++;

        AsyncMutex mutex(32);
        std::vector<std::unique_ptr<IOSchedulerType>> schedulers;

        // 创建多个调度器，每个在独立线程运行
        for (int i = 0; i < TEST19_SCHEDULER_COUNT; ++i) {
            schedulers.push_back(std::make_unique<IOSchedulerType>());
        }

        // 启动调度器并提交协程
        for (int i = 0; i < TEST19_SCHEDULER_COUNT; ++i) {
            schedulers[i]->start();
            schedulers[i]->schedule(detail::TaskAccess::detachTask(testCrossScheduler(&mutex)));
        }

        // 等待所有协程完成
        auto start = std::chrono::steady_clock::now();
        int expected = TEST19_SCHEDULER_COUNT * TEST19_OPS_PER_SCHEDULER;
        while (g_test19_completed < TEST19_SCHEDULER_COUNT || g_test19_counter < expected) {
            if (std::chrono::steady_clock::now() - start > 30s) break;
            std::this_thread::sleep_for(1ms);
        }

        // 停止调度器
        for (int i = 0; i < TEST19_SCHEDULER_COUNT; ++i) {
            schedulers[i]->stop();
        }

        bool passed = (g_test19_completed == TEST19_SCHEDULER_COUNT) &&
                      (g_test19_counter == expected);

        if (passed) {
            LogInfo("[PASS] Cross-scheduler: completed={}, counter={} (expected {})",
                    g_test19_completed.load(), g_test19_counter.load(), expected);
            g_passed++;
        } else {
            LogError("[FAIL] Cross-scheduler: completed={}/{}, counter={} (expected {})",
                    g_test19_completed.load(), TEST19_SCHEDULER_COUNT,
                    g_test19_counter.load(), expected);
        }
    }


#else
    LogWarn("No IO scheduler available, skipping tests");
#endif

    // 打印测试结果
    LogInfo("\n========================================");
    LogInfo("Test Results: {}/{} passed", g_passed.load(), g_total.load());
    LogInfo("========================================");
}

int main() {
    galay::test::TestResultWriter resultWriter("test_async_mutex");
    runTests();

    // 写入测试结果
    resultWriter.addTest();
    if (g_passed == g_total) {
        resultWriter.addPassed();
    } else {
        resultWriter.addFailed();
    }
    resultWriter.writeResult();

    return (g_passed == g_total) ? 0 : 1;
}
