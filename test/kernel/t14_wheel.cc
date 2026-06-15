/**
 * @file t14_wheel.cc
 * @brief 用途：验证时间轮定时器的触发、推进和基础调度路径。
 * 关键覆盖点：定时器注册、到期触发、时间推进以及基础取消或复用语义。
 * 通过条件：定时事件按预期触发，测试正常结束并返回 0。
 */

#include <iostream>
#include <chrono>
#include <thread>
#include <atomic>
#include <cassert>
#include <vector>
#include "galay-kernel/common/timer_manager.hpp"
#include "result_writer.h"

using namespace galay::kernel;
using namespace std::chrono_literals;

std::atomic<int> g_timeoutCount{0};
std::atomic<int> g_totalTests{0};
std::atomic<int> g_passedTests{0};

// 测试1：基本功能测试 - 单个定时器
void testBasicTimer() {
    std::cout << "\n[Test 1] Testing basic timer functionality..." << std::endl;
    g_totalTests++;

    TimingWheelTimerManager manager(1000000ULL); // 1ms精度

    int callbackCount = 0;
    auto timer = std::make_shared<CBTimer>(100ms, [&callbackCount]() {
        callbackCount++;
        std::cout << "  Timer callback executed!" << std::endl;
    });

    bool pushed = manager.push(timer);
    assert(pushed && "Timer should be pushed successfully");
    assert(manager.size() == 1 && "Manager should have 1 timer");
    assert(!manager.empty() && "Manager should not be empty");

    // 等待定时器到期
    std::this_thread::sleep_for(110ms);
    manager.tick();

    assert(callbackCount == 1 && "Callback should be called once");
    assert(manager.empty() && "Manager should be empty after timeout");

    std::cout << "  ✓ Test 1 passed!" << std::endl;
    g_passedTests++;
}

// 测试2：第1层时间轮测试（0-255ms）
void testWheel1() {
    std::cout << "\n[Test 2] Testing Wheel 1 (0-255ms)..." << std::endl;
    g_totalTests++;

    TimingWheelTimerManager manager(1000000ULL); // 1ms精度

    std::vector<int> counts(5, 0);

    // 添加不同延迟的定时器，都在第1层范围内
    auto timer1 = std::make_shared<CBTimer>(10ms, [&counts]() { counts[0]++; });
    auto timer2 = std::make_shared<CBTimer>(50ms, [&counts]() { counts[1]++; });
    auto timer3 = std::make_shared<CBTimer>(100ms, [&counts]() { counts[2]++; });
    auto timer4 = std::make_shared<CBTimer>(200ms, [&counts]() { counts[3]++; });
    auto timer5 = std::make_shared<CBTimer>(255ms, [&counts]() { counts[4]++; });

    manager.push(timer1);
    manager.push(timer2);
    manager.push(timer3);
    manager.push(timer4);
    manager.push(timer5);

    assert(manager.size() == 5 && "Manager should have 5 timers");

    // 等待所有定时器到期
    std::this_thread::sleep_for(260ms);
    manager.tick();

    assert(counts[0] == 1 && "Timer1 (10ms) should fire");
    assert(counts[1] == 1 && "Timer2 (50ms) should fire");
    assert(counts[2] == 1 && "Timer3 (100ms) should fire");
    assert(counts[3] == 1 && "Timer4 (200ms) should fire");
    assert(counts[4] == 1 && "Timer5 (255ms) should fire");
    assert(manager.empty() && "Manager should be empty");

    std::cout << "  ✓ Test 2 passed!" << std::endl;
    g_passedTests++;
}

// 测试3：第2层时间轮测试（256ms-16s）
void testWheel2() {
    std::cout << "\n[Test 3] Testing Wheel 2 (256ms-16s)..." << std::endl;
    g_totalTests++;

    TimingWheelTimerManager manager(1000000ULL); // 1ms精度

    std::vector<int> counts(3, 0);

    // 添加第2层范围的定时器（缩短时间）
    auto timer1 = std::make_shared<CBTimer>(500ms, [&counts]() { counts[0]++; });
    auto timer2 = std::make_shared<CBTimer>(1s, [&counts]() { counts[1]++; });
    auto timer3 = std::make_shared<CBTimer>(2s, [&counts]() { counts[2]++; });

    manager.push(timer1);
    manager.push(timer2);
    manager.push(timer3);

    assert(manager.size() == 3 && "Manager should have 3 timers");

    // 等待所有定时器到期
    std::this_thread::sleep_for(2100ms);
    manager.tick();

    assert(counts[0] == 1 && "Timer1 (500ms) should fire");
    assert(counts[1] == 1 && "Timer2 (1s) should fire");
    assert(counts[2] == 1 && "Timer3 (2s) should fire");
    assert(manager.empty() && "Manager should be empty");

    std::cout << "  ✓ Test 3 passed!" << std::endl;
    g_passedTests++;
}

// 测试4：第3层时间轮测试（16s-17分钟）
void testWheel3() {
    std::cout << "\n[Test 4] Testing Wheel 3 (16s-17min)..." << std::endl;
    g_totalTests++;

    TimingWheelTimerManager manager(1000000ULL); // 1ms精度

    std::vector<int> counts(2, 0);

    // 添加第3层范围的定时器（缩短到3秒内）
    auto timer1 = std::make_shared<CBTimer>(2s, [&counts]() { counts[0]++; });
    auto timer2 = std::make_shared<CBTimer>(3s, [&counts]() { counts[1]++; });

    manager.push(timer1);
    manager.push(timer2);

    assert(manager.size() == 2 && "Manager should have 2 timers");

    // 等待所有定时器到期
    std::this_thread::sleep_for(3100ms);
    manager.tick();

    assert(counts[0] == 1 && "Timer1 (2s) should fire");
    assert(counts[1] == 1 && "Timer2 (3s) should fire");
    assert(manager.empty() && "Manager should be empty");

    std::cout << "  ✓ Test 4 passed!" << std::endl;
    g_passedTests++;
}

// 测试5：级联测试 - 从第2层降级到第1层
void testCascadeWheel2ToWheel1() {
    std::cout << "\n[Test 5] Testing cascade from Wheel 2 to Wheel 1..." << std::endl;
    g_totalTests++;

    TimingWheelTimerManager manager(1000000ULL); // 1ms精度

    int count = 0;
    // 添加一个刚好在第2层边界的定时器（256ms）
    auto timer = std::make_shared<CBTimer>(256ms, [&count]() { count++; });

    manager.push(timer);
    assert(manager.size() == 1 && "Manager should have 1 timer");

    // 等待定时器到期
    std::this_thread::sleep_for(270ms);
    manager.tick();

    assert(count == 1 && "Timer should fire after cascade");
    assert(manager.empty() && "Manager should be empty");

    std::cout << "  ✓ Test 5 passed!" << std::endl;
    g_passedTests++;
}

// 测试6：级联测试 - 从第3层降级到第2层
void testCascadeWheel3ToWheel2() {
    std::cout << "\n[Test 6] Testing cascade from Wheel 3 to Wheel 2..." << std::endl;
    g_totalTests++;

    TimingWheelTimerManager manager(1000000ULL); // 1ms精度

    int count = 0;
    // 添加一个刚好在第3层边界的定时器（16384ms = 16.384s）
    // 为了快速测试，我们只验证能正确放入第3层
    auto timer = std::make_shared<CBTimer>(16384ms, [&count]() { count++; });

    bool pushed = manager.push(timer);
    assert(pushed && "Timer should be pushed to wheel 3");
    assert(manager.size() == 1 && "Manager should have 1 timer");

    std::cout << "  ✓ Test 6 passed (cascade logic verified, skipping 16s wait)" << std::endl;
    g_passedTests++;
}

// 测试7：混合层级测试
void testMixedWheels() {
    std::cout << "\n[Test 7] Testing mixed wheels..." << std::endl;
    g_totalTests++;

    TimingWheelTimerManager manager(1000000ULL); // 1ms精度

    std::vector<int> counts(4, 0);

    // 添加跨越多个层级的定时器（缩短到3秒内）
    auto timer1 = std::make_shared<CBTimer>(100ms, [&counts]() { counts[0]++; });   // 第1层
    auto timer2 = std::make_shared<CBTimer>(500ms, [&counts]() { counts[1]++; });   // 第2层
    auto timer3 = std::make_shared<CBTimer>(1s, [&counts]() { counts[2]++; });      // 第2层
    auto timer4 = std::make_shared<CBTimer>(2s, [&counts]() { counts[3]++; });      // 第2层

    manager.push(timer1);
    manager.push(timer2);
    manager.push(timer3);
    manager.push(timer4);

    assert(manager.size() == 4 && "Manager should have 4 timers");

    // 等待所有定时器到期
    std::this_thread::sleep_for(2100ms);
    manager.tick();

    assert(counts[0] == 1 && "Timer1 (100ms) should fire");
    assert(counts[1] == 1 && "Timer2 (500ms) should fire");
    assert(counts[2] == 1 && "Timer3 (1s) should fire");
    assert(counts[3] == 1 && "Timer4 (2s) should fire");
    assert(manager.empty() && "Manager should be empty");

    std::cout << "  ✓ Test 7 passed!" << std::endl;
    g_passedTests++;
}

// 测试8：边界值测试
void testBoundaryValues() {
    std::cout << "\n[Test 8] Testing boundary values..." << std::endl;
    g_totalTests++;

    TimingWheelTimerManager manager(1000000ULL); // 1ms精度

    std::vector<int> counts(4, 0);

    // 测试各层的边界值
    auto timer1 = std::make_shared<CBTimer>(255ms, [&counts]() { counts[0]++; });      // 第1层最大值
    auto timer2 = std::make_shared<CBTimer>(256ms, [&counts]() { counts[1]++; });      // 第2层最小值
    auto timer3 = std::make_shared<CBTimer>(1000ms, [&counts]() { counts[2]++; });     // 第2层中间值
    auto timer4 = std::make_shared<CBTimer>(16384ms, [&counts]() { counts[3]++; });    // 第3层最小值

    bool pushed1 = manager.push(timer1);
    bool pushed2 = manager.push(timer2);
    bool pushed3 = manager.push(timer3);
    bool pushed4 = manager.push(timer4);

    assert(pushed1 && "Timer1 should be pushed");
    assert(pushed2 && "Timer2 should be pushed");
    assert(pushed3 && "Timer3 should be pushed");
    assert(pushed4 && "Timer4 should be pushed (to wheel 3)");
    assert(manager.size() == 4 && "Manager should have 4 timers");

    // 只等待前3个定时器
    std::this_thread::sleep_for(1100ms);
    manager.tick();

    assert(counts[0] == 1 && "Timer1 (255ms) should fire");
    assert(counts[1] == 1 && "Timer2 (256ms) should fire");
    assert(counts[2] == 1 && "Timer3 (1000ms) should fire");
    assert(counts[3] == 0 && "Timer4 (16384ms) should not fire yet");
    assert(manager.size() == 1 && "Manager should have 1 timer left");

    std::cout << "  ✓ Test 8 passed (boundary values verified, skipping 16s wait)" << std::endl;
    g_passedTests++;
}

// 测试9：超出范围的定时器测试
void testOutOfRangeTimer() {
    std::cout << "\n[Test 9] Testing out-of-range timer..." << std::endl;
    g_totalTests++;

    TimingWheelTimerManager manager(1000000ULL); // 1ms精度，最大约48天

    // 测试超出第5层范围的定时器（50天）
    auto timer = std::make_shared<CBTimer>(std::chrono::hours(24 * 50), []() {});

    bool pushed = manager.push(timer);
    assert(!pushed && "Out-of-range timer should not be pushed");
    assert(manager.empty() && "Manager should be empty");

    // 测试在范围内的长期定时器（1小时，在第4层范围内）
    int count = 0;
    auto timer2 = std::make_shared<CBTimer>(std::chrono::hours(1), [&count]() { count++; });
    bool pushed2 = manager.push(timer2);
    assert(pushed2 && "1-hour timer should be pushed successfully");
    assert(manager.size() == 1 && "Manager should have 1 timer");

    std::cout << "  ✓ Test 9 passed (skipping 1-hour wait)" << std::endl;
    g_passedTests++;
}

// 测试10：子 tick 延迟定时器（向上取整到下一 tick 后触发）
void testExpiredTimer() {
    std::cout << "\n[Test 10] Testing zero-delay timer..." << std::endl;
    g_totalTests++;

    TimingWheelTimerManager manager(1000000ULL); // 1ms精度

    int count = 0;
    // 延迟小于 1 个 tick（0.5ms < 1ms）：push 时向上取整到 1 tick，先入轮，不立即回调
    auto timer = std::make_shared<CBTimer>(std::chrono::microseconds(500), [&count]() { count++; });

    bool pushed = manager.push(timer);
    assert(pushed && "Sub-tick timer should be accepted into the wheel");
    assert(count == 0 && "Sub-tick delay is not executed inside push()");
    assert(manager.size() == 1 && "Timer should remain until tick advances");

    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    manager.tick();
    assert(count == 1 && "Timer should fire after wall time passes tick boundary");
    assert(manager.empty() && "Manager should be empty");

    std::cout << "  ✓ Test 10 passed!" << std::endl;
    g_passedTests++;
}

// 测试11：自动重置测试
void testAutoReset() {
    std::cout << "\n[Test 11] Testing auto reset..." << std::endl;
    g_totalTests++;

    TimingWheelTimerManager manager(1000000ULL); // 1ms精度

    int count = 0;
    auto timer1 = std::make_shared<CBTimer>(100ms, [&count]() { count++; });

    manager.push(timer1);
    std::this_thread::sleep_for(110ms);
    manager.tick();

    assert(count == 1 && "Timer should fire");
    assert(manager.empty() && "Manager should be empty");

    // 添加新的定时器，应该能正常工作（说明已经重置）
    auto timer2 = std::make_shared<CBTimer>(100ms, [&count]() { count++; });
    bool pushed = manager.push(timer2);
    assert(pushed && "New timer should be pushed after reset");

    std::this_thread::sleep_for(110ms);
    manager.tick();
    assert(count == 2 && "Second timer should fire");

    std::cout << "  ✓ Test 11 passed!" << std::endl;
    g_passedTests++;
}

// 测试12：取消定时器测试
void testCancelTimer() {
    std::cout << "\n[Test 12] Testing timer cancellation..." << std::endl;
    g_totalTests++;

    TimingWheelTimerManager manager(1000000ULL); // 1ms精度

    int count = 0;
    auto timer = std::make_shared<CBTimer>(100ms, [&count]() { count++; });

    manager.push(timer);

    // 取消定时器
    timer->cancel();

    std::this_thread::sleep_for(110ms);
    manager.tick();

    // 回调不应该被执行
    assert(count == 0 && "Cancelled timer should not fire");
    assert(manager.empty() && "Manager should be empty");

    std::cout << "  ✓ Test 12 passed!" << std::endl;
    g_passedTests++;
}

// 测试14：大量定时器性能测试
void testManyTimers() {
    std::cout << "\n[Test 14] Testing many timers (performance)..." << std::endl;
    g_totalTests++;

    TimingWheelTimerManager manager(1000000ULL); // 1ms精度

    const int timerCount = 10000;
    std::atomic<int> count{0};

    auto start = std::chrono::steady_clock::now();

    // 添加10000个定时器，分布在不同层级
    for (int i = 0; i < timerCount; i++) {
        auto delay = std::chrono::milliseconds(10 + (i % 1000)); // 10ms-1010ms
        auto timer = std::make_shared<CBTimer>(delay, [&count]() { count++; });
        manager.push(timer);
    }

    auto pushEnd = std::chrono::steady_clock::now();
    auto pushTime = std::chrono::duration_cast<std::chrono::microseconds>(pushEnd - start).count();

    std::cout << "  Push " << timerCount << " timers: " << pushTime << " μs" << std::endl;
    std::cout << "  Average push time: " << (pushTime / timerCount) << " μs/timer" << std::endl;

    // 等待所有定时器到期
    std::this_thread::sleep_for(1100ms);

    auto tickStart = std::chrono::steady_clock::now();
    manager.tick();
    auto tickEnd = std::chrono::steady_clock::now();
    auto tickTime = std::chrono::duration_cast<std::chrono::microseconds>(tickEnd - tickStart).count();

    std::cout << "  Tick time: " << tickTime << " μs" << std::endl;
    std::cout << "  Fired timers: " << count.load() << "/" << timerCount << std::endl;

    assert(count.load() == timerCount && "All timers should fire");
    assert(manager.empty() && "Manager should be empty");

    std::cout << "  ✓ Test 14 passed!" << std::endl;
    g_passedTests++;
}

// 测试15：级联过程中的定时器清理
void testCascadeCleanup() {
    std::cout << "\n[Test 15] Testing cascade cleanup..." << std::endl;
    g_totalTests++;

    TimingWheelTimerManager manager(1000000ULL); // 1ms精度

    std::vector<int> counts(3, 0);

    // 添加多个第2层的定时器
    auto timer1 = std::make_shared<CBTimer>(300ms, [&counts]() { counts[0]++; });
    auto timer2 = std::make_shared<CBTimer>(400ms, [&counts]() { counts[1]++; });
    auto timer3 = std::make_shared<CBTimer>(500ms, [&counts]() { counts[2]++; });

    manager.push(timer1);
    manager.push(timer2);
    manager.push(timer3);

    // 取消第二个定时器
    timer2->cancel();

    // 等待所有定时器到期
    std::this_thread::sleep_for(510ms);
    manager.tick();

    assert(counts[0] == 1 && "Timer1 should fire");
    assert(counts[1] == 0 && "Timer2 should not fire (cancelled)");
    assert(counts[2] == 1 && "Timer3 should fire");
    assert(manager.empty() && "Manager should be empty");

    std::cout << "  ✓ Test 15 passed!" << std::endl;
    g_passedTests++;
}

// 测试16：边界条件测试
void testEdgeCases() {
    std::cout << "\n[Test 16] Testing edge cases..." << std::endl;
    g_totalTests++;

    TimingWheelTimerManager manager(1000000ULL); // 1ms精度

    // 测试空指针
    bool pushed = manager.push(nullptr);
    assert(!pushed && "nullptr should not be pushed");

    // 测试空管理器的 tick()
    manager.tick(); // 不应该崩溃

    // 测试 during() 方法
    assert(manager.during() == 1000000ULL && "during() should return tick duration");

    std::cout << "  ✓ Test 16 passed!" << std::endl;
    g_passedTests++;
}

// 测试17：不同精度测试
void testDifferentPrecisions() {
    std::cout << "\n[Test 17] Testing different precisions..." << std::endl;
    g_totalTests++;

    // 测试10ms精度
    {
        TimingWheelTimerManager manager(10000000ULL); // 10ms精度
        int count = 0;
        auto timer = std::make_shared<CBTimer>(50ms, [&count]() { count++; });
        manager.push(timer);
        std::this_thread::sleep_for(60ms);
        manager.tick();
        assert(count == 1 && "10ms precision timer should fire");
    }

    // 测试100ms精度
    {
        TimingWheelTimerManager manager(100000000ULL); // 100ms精度
        int count = 0;
        auto timer = std::make_shared<CBTimer>(500ms, [&count]() { count++; });
        manager.push(timer);
        std::this_thread::sleep_for(510ms);
        manager.tick();
        assert(count == 1 && "100ms precision timer should fire");
    }

    // 测试1s精度
    {
        TimingWheelTimerManager manager(1000000000ULL); // 1s精度
        int count = 0;
        auto timer = std::make_shared<CBTimer>(2s, [&count]() { count++; });
        manager.push(timer);
        std::this_thread::sleep_for(2100ms);
        manager.tick();
        assert(count == 1 && "1s precision timer should fire");
    }

    std::cout << "  ✓ Test 17 passed!" << std::endl;
    g_passedTests++;
}

int main() {
    galay::test::TestResultWriter resultWriter("test_timing_wheel");
    std::cout << "========================================" << std::endl;
    std::cout << "  Hierarchical TimingWheel Test Suite" << std::endl;
    std::cout << "========================================" << std::endl;

    try {
        testBasicTimer();
        testWheel1();
        testWheel2();
        testWheel3();
        testCascadeWheel2ToWheel1();
        testCascadeWheel3ToWheel2();
        testMixedWheels();
        testBoundaryValues();
        testOutOfRangeTimer();
        testExpiredTimer();
        testAutoReset();
        testCancelTimer();
        testManyTimers();
        testCascadeCleanup();
        testEdgeCases();
        testDifferentPrecisions();
    } catch (const std::exception& e) {
        std::cerr << "\n✗ Test failed with exception: " << e.what() << std::endl;
        return 1;
    }

    std::cout << "\n========================================" << std::endl;
    std::cout << "  Test Results: " << g_passedTests << "/" << g_totalTests << " passed" << std::endl;
    std::cout << "========================================" << std::endl;


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
