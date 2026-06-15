/**
 * @file t16_timer.cc
 * @brief 用途：验证 `TimerScheduler` 在多线程与压力场景下的正确性。
 * 关键覆盖点：基础定时触发、多线程并发添加、取消操作和批量压力用例。
 * 通过条件：各类定时器用例断言成立，测试进程返回 0。
 */

#include <iostream>
#include <chrono>
#include <thread>
#include <atomic>
#include <cassert>
#include <vector>
#include <random>
#include "galay-kernel/core/timer_scheduler.h"
#include "result_writer.h"

using namespace galay::kernel;
using namespace std::chrono_literals;

std::atomic<int> g_totalTests{0};
std::atomic<int> g_passedTests{0};

// 测试1：基本功能测试
void testBasicFunctionality() {
    std::cout << "\n[Test 1] Testing basic functionality..." << std::endl;
    g_totalTests++;

    auto* scheduler = TimerScheduler::getInstance();
    scheduler->start();

    std::atomic<int> count{0};
    auto timer = std::make_shared<CBTimer>(100ms, [&count]() {
        count++;
        std::cout << "  Timer callback executed!" << std::endl;
    });

    bool added = scheduler->addTimer(timer);
    assert(added && "Timer should be added successfully");

    // 等待定时器触发
    std::this_thread::sleep_for(150ms);

    assert(count.load() == 1 && "Callback should be called once");

    scheduler->stop();
    std::cout << "  ✓ Test 1 passed!" << std::endl;
    g_passedTests++;
}

// 测试2：多线程并发添加定时器
void testConcurrentAdd() {
    std::cout << "\n[Test 2] Testing concurrent timer addition..." << std::endl;
    g_totalTests++;

    auto* scheduler = TimerScheduler::getInstance();
    scheduler->start();

    const int numThreads = 8;
    const int timersPerThread = 100;
    std::atomic<int> totalFired{0};

    std::vector<std::thread> threads;

    for (int t = 0; t < numThreads; t++) {
        threads.emplace_back([&]() {
            for (int i = 0; i < timersPerThread; i++) {
                auto delay = std::chrono::milliseconds(50 + (i % 100));
                auto timer = std::make_shared<CBTimer>(delay, [&totalFired]() {
                    totalFired.fetch_add(1, std::memory_order_relaxed);
                });
                scheduler->addTimer(timer);
            }
        });
    }

    // 等待所有线程完成添加
    for (auto& t : threads) {
        t.join();
    }

    std::cout << "  Added " << (numThreads * timersPerThread) << " timers from "
              << numThreads << " threads" << std::endl;

    // 等待所有定时器触发
    std::this_thread::sleep_for(200ms);

    int fired = totalFired.load();
    std::cout << "  Fired: " << fired << "/" << (numThreads * timersPerThread) << std::endl;

    assert(fired == numThreads * timersPerThread && "All timers should fire");

    scheduler->stop();
    std::cout << "  ✓ Test 2 passed!" << std::endl;
    g_passedTests++;
}

// 测试3：高并发压力测试
void testHighConcurrency() {
    std::cout << "\n[Test 3] Testing high concurrency stress..." << std::endl;
    g_totalTests++;

    auto* scheduler = TimerScheduler::getInstance();
    scheduler->start();

    const int numThreads = 16;
    const int timersPerThread = 1000;
    std::atomic<int> totalFired{0};
    std::atomic<bool> startFlag{false};

    std::vector<std::thread> threads;

    // 创建线程，等待同时开始
    for (int t = 0; t < numThreads; t++) {
        threads.emplace_back([&]() {
            // 等待开始信号
            while (!startFlag.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }

            std::random_device rd;
            std::mt19937 gen(rd());
            std::uniform_int_distribution<> dis(10, 500);

            for (int i = 0; i < timersPerThread; i++) {
                auto delay = std::chrono::milliseconds(dis(gen));
                auto timer = std::make_shared<CBTimer>(delay, [&totalFired]() {
                    totalFired.fetch_add(1, std::memory_order_relaxed);
                });
                scheduler->addTimer(timer);
            }
        });
    }

    auto start = std::chrono::steady_clock::now();

    // 发送开始信号
    startFlag.store(true, std::memory_order_release);

    // 等待所有线程完成添加
    for (auto& t : threads) {
        t.join();
    }

    auto addEnd = std::chrono::steady_clock::now();
    auto addTime = std::chrono::duration_cast<std::chrono::milliseconds>(addEnd - start).count();

    std::cout << "  Added " << (numThreads * timersPerThread) << " timers in "
              << addTime << " ms" << std::endl;
    std::cout << "  Throughput: " << (numThreads * timersPerThread * 1000 / std::max(1LL, static_cast<long long>(addTime)))
              << " timers/sec" << std::endl;

    // 等待所有定时器触发
    std::this_thread::sleep_for(600ms);

    int fired = totalFired.load();
    std::cout << "  Fired: " << fired << "/" << (numThreads * timersPerThread) << std::endl;

    assert(fired == numThreads * timersPerThread && "All timers should fire");

    scheduler->stop();
    std::cout << "  ✓ Test 3 passed!" << std::endl;
    g_passedTests++;
}

// 测试4：定时器取消测试
void testTimerCancellation() {
    std::cout << "\n[Test 4] Testing timer cancellation..." << std::endl;
    g_totalTests++;

    auto* scheduler = TimerScheduler::getInstance();
    scheduler->start();

    std::atomic<int> firedCount{0};
    std::atomic<int> cancelledCount{0};

    std::vector<Timer::ptr> timers;

    // 添加100个定时器
    for (int i = 0; i < 100; i++) {
        auto timer = std::make_shared<CBTimer>(200ms, [&firedCount]() {
            firedCount.fetch_add(1, std::memory_order_relaxed);
        });
        timers.push_back(timer);
        scheduler->addTimer(timer);
    }

    // 取消一半的定时器
    for (int i = 0; i < 50; i++) {
        timers[i]->cancel();
        cancelledCount++;
    }

    std::cout << "  Added 100 timers, cancelled 50" << std::endl;

    // 等待定时器触发
    std::this_thread::sleep_for(250ms);

    int fired = firedCount.load();
    std::cout << "  Fired: " << fired << " (expected: 50)" << std::endl;

    assert(fired == 50 && "Only non-cancelled timers should fire");

    scheduler->stop();
    std::cout << "  ✓ Test 4 passed!" << std::endl;
    g_passedTests++;
}

// 测试5：批量添加测试
void testBatchAdd() {
    std::cout << "\n[Test 5] Testing batch timer addition..." << std::endl;
    g_totalTests++;

    auto* scheduler = TimerScheduler::getInstance();
    scheduler->start();

    std::atomic<int> totalFired{0};
    const int batchSize = 1000;

    std::vector<Timer::ptr> timers;
    timers.reserve(batchSize);

    for (int i = 0; i < batchSize; i++) {
        auto delay = std::chrono::milliseconds(50 + (i % 100));
        auto timer = std::make_shared<CBTimer>(delay, [&totalFired]() {
            totalFired.fetch_add(1, std::memory_order_relaxed);
        });
        timers.push_back(timer);
    }

    auto start = std::chrono::steady_clock::now();
    size_t added = scheduler->addTimerBatch(timers);
    auto end = std::chrono::steady_clock::now();
    auto addTime = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();

    std::cout << "  Batch added " << added << " timers in " << addTime << " μs" << std::endl;

    assert(added == batchSize && "All timers should be added");

    // 等待所有定时器触发
    std::this_thread::sleep_for(200ms);

    int fired = totalFired.load();
    std::cout << "  Fired: " << fired << "/" << batchSize << std::endl;

    assert(fired == batchSize && "All timers should fire");

    scheduler->stop();
    std::cout << "  ✓ Test 5 passed!" << std::endl;
    g_passedTests++;
}

// 测试6：启动停止测试
void testStartStop() {
    std::cout << "\n[Test 6] Testing start/stop cycles..." << std::endl;
    g_totalTests++;

    auto* scheduler = TimerScheduler::getInstance();

    for (int cycle = 0; cycle < 3; cycle++) {
        scheduler->start();
        assert(scheduler->isRunning() && "Scheduler should be running");

        std::atomic<int> count{0};
        auto timer = std::make_shared<CBTimer>(50ms, [&count]() {
            count++;
        });
        scheduler->addTimer(timer);

        std::this_thread::sleep_for(100ms);
        assert(count.load() == 1 && "Timer should fire");

        scheduler->stop();
        assert(!scheduler->isRunning() && "Scheduler should be stopped");

        std::cout << "  Cycle " << (cycle + 1) << " completed" << std::endl;
    }

    std::cout << "  ✓ Test 6 passed!" << std::endl;
    g_passedTests++;
}

// 测试7：边界条件测试
void testEdgeCases() {
    std::cout << "\n[Test 7] Testing edge cases..." << std::endl;
    g_totalTests++;

    auto* scheduler = TimerScheduler::getInstance();

    // 测试未启动时添加定时器
    bool added = scheduler->addTimer(std::make_shared<CBTimer>(100ms, []() {}));
    assert(!added && "Should not add timer when not running");

    // 测试空指针
    scheduler->start();
    added = scheduler->addTimer(nullptr);
    assert(!added && "Should not add nullptr");

    // 测试零延迟定时器
    std::atomic<int> count{0};
    auto timer = std::make_shared<CBTimer>(0ms, [&count]() {
        count++;
    });
    scheduler->addTimer(timer);
    std::this_thread::sleep_for(50ms);
    assert(count.load() == 1 && "Zero-delay timer should fire immediately");

    scheduler->stop();
    std::cout << "  ✓ Test 7 passed!" << std::endl;
    g_passedTests++;
}

// 测试8：并发添加和取消
void testConcurrentAddAndCancel() {
    std::cout << "\n[Test 8] Testing concurrent add and cancel..." << std::endl;
    g_totalTests++;

    auto* scheduler = TimerScheduler::getInstance();
    scheduler->start();

    const int numTimers = 1000;
    std::atomic<int> firedCount{0};
    std::vector<Timer::ptr> timers(numTimers);

    // 线程1：添加定时器
    std::thread adder([&]() {
        for (int i = 0; i < numTimers; i++) {
            auto timer = std::make_shared<CBTimer>(100ms, [&firedCount]() {
                firedCount.fetch_add(1, std::memory_order_relaxed);
            });
            timers[i] = timer;
            scheduler->addTimer(timer);
        }
    });

    // 线程2：取消部分定时器
    std::thread canceller([&]() {
        std::this_thread::sleep_for(10ms);  // 等待一些定时器被添加
        for (int i = 0; i < numTimers; i += 2) {
            if (timers[i]) {
                timers[i]->cancel();
            }
        }
    });

    adder.join();
    canceller.join();

    // 等待定时器触发
    std::this_thread::sleep_for(150ms);

    int fired = firedCount.load();
    std::cout << "  Fired: " << fired << " (expected ~500)" << std::endl;

    // 由于并发，实际触发数可能略有不同
    assert(fired >= 400 && fired <= 600 && "Approximately half should fire");

    scheduler->stop();
    std::cout << "  ✓ Test 8 passed!" << std::endl;
    g_passedTests++;
}

int main() {
    galay::test::TestResultWriter resultWriter("test_timer_scheduler");
    std::cout << "==========================================" << std::endl;
    std::cout << "  TimerScheduler Multi-threaded Test Suite" << std::endl;
    std::cout << "==========================================" << std::endl;

    try {
        testBasicFunctionality();
        testConcurrentAdd();
        testHighConcurrency();
        testTimerCancellation();
        testBatchAdd();
        testStartStop();
        testEdgeCases();
        testConcurrentAddAndCancel();
    } catch (const std::exception& e) {
        std::cerr << "\n✗ Test failed with exception: " << e.what() << std::endl;
        return 1;
    }

    std::cout << "\n==========================================" << std::endl;
    std::cout << "  Test Results: " << g_passedTests << "/" << g_totalTests << " passed" << std::endl;
    std::cout << "==========================================" << std::endl;

    // 写入测试结果
    resultWriter.addTest();
    if (g_passedTests == g_totalTests) {
        resultWriter.addPassed();
    } else {
        resultWriter.addFailed();
    }
    resultWriter.writeResult();

    return (g_passedTests == g_totalTests) ? 0 : 1;
}
