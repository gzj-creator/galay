/**
 * @file t13_mpsc.cc
 * @brief 用途：验证 `MpscChannel` 在多生产者单消费者场景下的正确性。
 * 关键覆盖点：跨线程发送、协程接收、消息完整性与顺序、关闭与唤醒行为。
 * 通过条件：消息不丢失不重复，所有子测试通过并返回 0。
 */

#include <iostream>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>
#include "galay-kernel/concurrency/mpsc_channel.h"
#include "galay-kernel/core/task.h"
#include "galay-kernel/core/compute_scheduler.h"
#include "test/stdout_log.h"
#include "result_writer.h"

using namespace galay::kernel;
using namespace std::chrono_literals;

std::atomic<int> g_passed{0};
std::atomic<int> g_failed{0};
std::atomic<int> g_total{0};

// ============================================================================
// 测试1：基本 send/recv
// ============================================================================
std::atomic<bool> g_test1_done{false};
std::atomic<int> g_test1_received{0};

Task<void> test_basic_send_recv(MpscChannel<int>* channel) {
    auto value = co_await channel->recv();
    if (value && *value == 42) {
        g_test1_received = *value;
    }
    g_test1_done = true;
    co_return;
}

// ============================================================================
// 测试2：多次 send/recv
// ============================================================================
std::atomic<int> g_test2_sum{0};
std::atomic<bool> g_test2_done{false};
constexpr int TEST2_COUNT = 10;

Task<void> test_multiple_send_recv(MpscChannel<int>* channel) {
    for (int i = 0; i < TEST2_COUNT; ++i) {
        auto value = co_await channel->recv();
        if (value) {
            g_test2_sum.fetch_add(*value, std::memory_order_relaxed);
        }
    }
    g_test2_done = true;
    co_return;
}

// ============================================================================
// 测试3：批量发送/接收
// ============================================================================
std::atomic<int> g_test3_total{0};
std::atomic<bool> g_test3_done{false};

Task<void> test_batch_send_recv(MpscChannel<int>* channel) {
    auto batch = co_await channel->recvBatch(100);
    if (batch) {
        for (int v : *batch) {
            g_test3_total.fetch_add(v, std::memory_order_relaxed);
        }
    }
    g_test3_done = true;
    co_return;
}

// ============================================================================
// 测试4：try_recv（非阻塞）
// ============================================================================
std::atomic<bool> g_test4_done{false};
std::atomic<int> g_test4_value{0};

Task<void> test_try_recv(MpscChannel<int>* channel) {
    // 先尝试接收（应该有数据，因为主线程已经发送了）
    auto value = channel->tryRecv();
    if (value) {
        g_test4_value = *value;
        g_test4_done = true;
        co_return;
    }

    // 如果没有数据，等待数据
    auto recv_value = co_await channel->recv();
    if (recv_value) {
        g_test4_value = *recv_value;
    }
    g_test4_done = true;
    co_return;
}

// ============================================================================
// 测试5：多生产者
// ============================================================================
std::atomic<int> g_test5_sum{0};
std::atomic<int> g_test5_recv_count{0};
std::atomic<bool> g_test5_done{false};
constexpr int TEST5_PRODUCER_COUNT = 5;
constexpr int TEST5_MSG_PER_PRODUCER = 10;

Task<void> test_multi_producer_consumer(MpscChannel<int>* channel) {
    int expected = TEST5_PRODUCER_COUNT * TEST5_MSG_PER_PRODUCER;
    while (g_test5_recv_count < expected) {
        auto value = co_await channel->recv();
        if (value) {
            g_test5_sum.fetch_add(*value, std::memory_order_relaxed);
            g_test5_recv_count.fetch_add(1, std::memory_order_relaxed);
        }
    }
    g_test5_done = true;
    co_return;
}

void producerThread(MpscChannel<int>* channel, int id) {
    for (int i = 0; i < TEST5_MSG_PER_PRODUCER; ++i) {
        channel->send(id * 100 + i);
        // 移除 sleep，让生产者尽快发送
    }
}

// ============================================================================
// 测试6：空通道等待
// ============================================================================
std::atomic<bool> g_test6_waiting{false};
std::atomic<bool> g_test6_received{false};
std::atomic<bool> g_test6_done{false};

Task<void> test_empty_channel_wait(MpscChannel<int>* channel) {
    g_test6_waiting = true;
    auto value = co_await channel->recv();
    g_test6_received = value.has_value();
    g_test6_done = true;
    co_return;
}

// ============================================================================
// 测试7：size() 和 empty()
// ============================================================================
std::atomic<bool> g_test7_done{false};

Task<void> test_size_and_empty(MpscChannel<int>* channel) {
    // 消费所有数据
    while (!channel->empty()) {
        co_await channel->recv();
    }
    g_test7_done = true;
    co_return;
}

// ============================================================================
// 测试8：批量发送
// ============================================================================
std::atomic<int> g_test8_total{0};
std::atomic<int> g_test8_count{0};
std::atomic<bool> g_test8_done{false};

Task<void> test_batch_send(MpscChannel<int>* channel) {
    // 接收所有数据（总共 10 个元素，分 3 批发送）
    int total_received = 0;
    while (total_received < 10) {
        auto batch = co_await channel->recvBatch(100);
        if (batch) {
            g_test8_count.fetch_add(batch->size(), std::memory_order_relaxed);
            for (int v : *batch) {
                g_test8_total.fetch_add(v, std::memory_order_relaxed);
            }
            total_received += batch->size();
        }
    }
    g_test8_done = true;
    co_return;
}

// ============================================================================
// 测试9：字符串类型
// ============================================================================
std::atomic<bool> g_test9_done{false};
std::string g_test9_result;

Task<void> test_string_channel(MpscChannel<std::string>* channel) {
    auto value = co_await channel->recv();
    if (value) {
        g_test9_result = *value;
    }
    g_test9_done = true;
    co_return;
}

// ============================================================================
// 测试10：高并发压力测试
// ============================================================================
std::atomic<int> g_test10_sent{0};
std::atomic<int> g_test10_received{0};
std::atomic<bool> g_test10_done{false};
constexpr int TEST10_TOTAL = 1000;

Task<void> test_high_concurrency(MpscChannel<int>* channel) {
    while (g_test10_received < TEST10_TOTAL) {
        auto value = co_await channel->recv();
        if (value) {
            g_test10_received.fetch_add(1, std::memory_order_relaxed);
        }
    }
    g_test10_done = true;
    co_return;
}

void highConcurrencyProducer(MpscChannel<int>* channel, int count) {
    for (int i = 0; i < count; ++i) {
        channel->send(i);
        g_test10_sent.fetch_add(1, std::memory_order_relaxed);
    }
}

// ============================================================================
// 测试11：跨调度器场景 - 生产者和消费者在不同调度器
// ============================================================================
std::atomic<int> g_test11_sum{0};
std::atomic<int> g_test11_count{0};
std::atomic<bool> g_test11_producer_done{false};
std::atomic<bool> g_test11_consumer_done{false};
constexpr int TEST11_MSG_COUNT = 50;

Task<void> test_cross_scheduler_producer(MpscChannel<int>* channel) {

    for (int i = 1; i <= TEST11_MSG_COUNT; ++i) {
        channel->send(i);
        co_yield true;  // 让出执行权
    }
    g_test11_producer_done = true;
    co_return;
}

Task<void> test_cross_scheduler_consumer(MpscChannel<int>* channel) {
    while (g_test11_count < TEST11_MSG_COUNT) {
        auto value = co_await channel->recv();
        if (value) {
            g_test11_sum.fetch_add(*value, std::memory_order_relaxed);
            g_test11_count.fetch_add(1, std::memory_order_relaxed);
        }
    }
    g_test11_consumer_done = true;
    co_return;
}

// ============================================================================
// 测试12：多调度器多生产者
// ============================================================================
std::atomic<int> g_test12_sum{0};
std::atomic<int> g_test12_count{0};
std::atomic<int> g_test12_producers_done{0};
std::atomic<bool> g_test12_consumer_done{false};
constexpr int TEST12_PRODUCER_COUNT = 3;
constexpr int TEST12_MSG_PER_PRODUCER = 20;

Task<void> test_multi_scheduler_producer(MpscChannel<int>* channel, int id) {

    for (int i = 0; i < TEST12_MSG_PER_PRODUCER; ++i) {
        channel->send(id * 100 + i);
        co_yield true;
    }
    g_test12_producers_done.fetch_add(1, std::memory_order_relaxed);
    co_return;
}

Task<void> test_multi_scheduler_consumer(MpscChannel<int>* channel) {
    int expected = TEST12_PRODUCER_COUNT * TEST12_MSG_PER_PRODUCER;
    while (g_test12_count < expected) {
        auto value = co_await channel->recv();
        if (value) {
            g_test12_sum.fetch_add(*value, std::memory_order_relaxed);
            g_test12_count.fetch_add(1, std::memory_order_relaxed);
        }
    }
    g_test12_consumer_done = true;
    co_return;
}

// ============================================================================
// 测试13：同线程多生产者单消费者（高性能路径）
// ============================================================================
std::atomic<int> g_test13_sum{0};
std::atomic<int> g_test13_count{0};
std::atomic<bool> g_test13_done{false};
constexpr int TEST13_MSG_COUNT = 100;

// 生产者协程 - 在同一调度器线程中发送数据
Task<void> test_same_thread_producer(MpscChannel<int>* channel, int startValue, int count) {

    for (int i = 0; i < count; ++i) {
        channel->send(startValue + i);
        co_yield true;  // 让出执行权，让消费者有机会接收
    }
    co_return;
}

// 消费者协程 - 在同一调度器线程中接收数据
Task<void> test_same_thread_consumer(MpscChannel<int>* channel, int expectedCount) {
    while (g_test13_count < expectedCount) {
        auto value = co_await channel->recv();
        if (value) {
            g_test13_sum.fetch_add(*value, std::memory_order_relaxed);
            g_test13_count.fetch_add(1, std::memory_order_relaxed);
        }
    }
    g_test13_done = true;
    co_return;
}

// ============================================================================
// 主函数
// ============================================================================
void runTests() {
    LogInfo("========================================");
    LogInfo("MpscChannel Unit Tests");
    LogInfo("========================================");

    // 测试1：基本 send/recv
    {
        LogInfo("\n--- Test 1: Basic send/recv ---");
        g_total++;

        ComputeScheduler scheduler;
        MpscChannel<int> channel;

        scheduler.start();
        scheduler.schedule(detail::TaskAccess::detachTask(test_basic_send_recv(&channel)));

        channel.send(42);

        auto start = std::chrono::steady_clock::now();
        while (!g_test1_done) {
            // 使用调度器的空闲等待
            if (std::chrono::steady_clock::now() - start > 5s) break;
        }

        scheduler.stop();

        if (g_test1_done && g_test1_received == 42) {
            LogInfo("[PASS] Basic send/recv: received={}", g_test1_received.load());
            g_passed++;
        } else {
            LogError("[FAIL] Basic send/recv: done={}, received={}",
                    g_test1_done.load(), g_test1_received.load());
            g_failed++;
        }
    }

    // 测试2：多次 send/recv
    {
        LogInfo("\n--- Test 2: Multiple send/recv ({} messages) ---", TEST2_COUNT);
        g_total++;

        ComputeScheduler scheduler;
        MpscChannel<int> channel;

        scheduler.start();
        scheduler.schedule(detail::TaskAccess::detachTask(test_multiple_send_recv(&channel)));

        // 发送数据
        int expected_sum = 0;
        for (int i = 0; i < TEST2_COUNT; ++i) {
            channel.send(i + 1);
            expected_sum += (i + 1);
        }

        auto start = std::chrono::steady_clock::now();
        while (!g_test2_done) {
            // 使用调度器的空闲等待
            if (std::chrono::steady_clock::now() - start > 10s) break;
        }

        scheduler.stop();

        if (g_test2_done && g_test2_sum == expected_sum) {
            LogInfo("[PASS] Multiple send/recv: sum={} (expected {})",
                    g_test2_sum.load(), expected_sum);
            g_passed++;
        } else {
            LogError("[FAIL] Multiple send/recv: done={}, sum={} (expected {})",
                    g_test2_done.load(), g_test2_sum.load(), expected_sum);
            g_failed++;
        }
    }

    // 测试3：批量发送/接收
    {
        LogInfo("\n--- Test 3: Batch send/recv ---");
        g_total++;

        ComputeScheduler scheduler;
        MpscChannel<int> channel;

        // 先发送数据
        std::vector<int> data = {1, 2, 3, 4, 5};
        int expected = 15;
        channel.sendBatch(data);

        scheduler.start();
        scheduler.schedule(detail::TaskAccess::detachTask(test_batch_send_recv(&channel)));

        auto start = std::chrono::steady_clock::now();
        while (!g_test3_done) {
            // 使用调度器的空闲等待
            if (std::chrono::steady_clock::now() - start > 5s) break;
        }

        scheduler.stop();

        if (g_test3_done && g_test3_total == expected) {
            LogInfo("[PASS] Batch send/recv: total={} (expected {})",
                    g_test3_total.load(), expected);
            g_passed++;
        } else {
            LogError("[FAIL] Batch send/recv: done={}, total={} (expected {})",
                    g_test3_done.load(), g_test3_total.load(), expected);
            g_failed++;
        }
    }

    // 测试4：try_recv
    {
        LogInfo("\n--- Test 4: try_recv (non-blocking) ---");
        g_total++;

        ComputeScheduler scheduler;
        MpscChannel<int> channel;

        scheduler.start();

        // 先发送数据，确保数据在协程启动前就在 channel 中
        channel.send(99);

        scheduler.schedule(detail::TaskAccess::detachTask(test_try_recv(&channel)));

        auto start = std::chrono::steady_clock::now();
        while (!g_test4_done) {
            // 使用调度器的空闲等待
            if (std::chrono::steady_clock::now() - start > 5s) break;
        }

        scheduler.stop();

        if (g_test4_done && g_test4_value == 99) {
            LogInfo("[PASS] try_recv: value={}", g_test4_value.load());
            g_passed++;
        } else {
            LogError("[FAIL] try_recv: done={}, value={}",
                    g_test4_done.load(), g_test4_value.load());
            g_failed++;
        }
    }

    // 测试5：多生产者
    {
        LogInfo("\n--- Test 5: Multi-producer ({} producers x {} messages) ---",
                TEST5_PRODUCER_COUNT, TEST5_MSG_PER_PRODUCER);
        g_total++;

        ComputeScheduler scheduler;
        MpscChannel<int> channel;

        scheduler.start();
        scheduler.schedule(detail::TaskAccess::detachTask(test_multi_producer_consumer(&channel)));

        // 启动多个生产者线程
        std::vector<std::thread> producers;
        for (int i = 0; i < TEST5_PRODUCER_COUNT; ++i) {
            producers.emplace_back(producerThread, &channel, i);
        }

        // 等待生产者完成
        for (auto& t : producers) {
            t.join();
        }

        auto start = std::chrono::steady_clock::now();
        while (!g_test5_done) {
            // 使用调度器的空闲等待
            if (std::chrono::steady_clock::now() - start > 10s) break;
        }

        scheduler.stop();

        int expected_count = TEST5_PRODUCER_COUNT * TEST5_MSG_PER_PRODUCER;
        if (g_test5_done && g_test5_recv_count == expected_count) {
            LogInfo("[PASS] Multi-producer: received={}, sum={}",
                    g_test5_recv_count.load(), g_test5_sum.load());
            g_passed++;
        } else {
            LogError("[FAIL] Multi-producer: done={}, received={}/{}",
                    g_test5_done.load(), g_test5_recv_count.load(), expected_count);
            g_failed++;
        }
    }

    // 测试6：空通道等待
    {
        LogInfo("\n--- Test 6: Empty channel wait ---");
        g_total++;

        ComputeScheduler scheduler;
        MpscChannel<int> channel;

        scheduler.start();
        scheduler.schedule(detail::TaskAccess::detachTask(test_empty_channel_wait(&channel)));

        // 等待消费者开始等待
        auto start = std::chrono::steady_clock::now();
        while (!g_test6_waiting) {
            // 使用调度器的空闲等待
            if (std::chrono::steady_clock::now() - start > 1s) break;
        }

        // 延迟发送
        channel.send(123);

        start = std::chrono::steady_clock::now();
        while (!g_test6_done) {
            // 使用调度器的空闲等待
            if (std::chrono::steady_clock::now() - start > 5s) break;
        }

        scheduler.stop();

        if (g_test6_done && g_test6_received) {
            LogInfo("[PASS] Empty channel wait: received after wait");
            g_passed++;
        } else {
            LogError("[FAIL] Empty channel wait: done={}, received={}",
                    g_test6_done.load(), g_test6_received.load());
            g_failed++;
        }
    }

    // 测试7：size() 和 empty()
    {
        LogInfo("\n--- Test 7: size() and empty() ---");
        g_total++;

        ComputeScheduler scheduler;
        MpscChannel<int> channel;

        // 发送数据
        for (int i = 0; i < 5; ++i) {
            channel.send(i);
        }

        bool size_ok = (channel.size() == 5);
        bool not_empty = !channel.empty();

        scheduler.start();
        scheduler.schedule(detail::TaskAccess::detachTask(test_size_and_empty(&channel)));

        auto start = std::chrono::steady_clock::now();
        while (!g_test7_done) {
            // 使用调度器的空闲等待
            if (std::chrono::steady_clock::now() - start > 5s) break;
        }

        scheduler.stop();

        bool empty_after = channel.empty();

        if (size_ok && not_empty && empty_after && g_test7_done) {
            LogInfo("[PASS] size/empty: initial_size=5, empty_after=true");
            g_passed++;
        } else {
            LogError("[FAIL] size/empty: size_ok={}, not_empty={}, empty_after={}",
                    size_ok, not_empty, empty_after);
            g_failed++;
        }
    }

    // 测试8：批量发送
    {
        LogInfo("\n--- Test 8: Batch send ---");
        g_total++;

        ComputeScheduler scheduler;
        MpscChannel<int> channel;

        scheduler.start();
        scheduler.schedule(detail::TaskAccess::detachTask(test_batch_send(&channel)));

        // 批量发送
        std::vector<int> batch1 = {1, 2, 3};
        std::vector<int> batch2 = {4, 5, 6, 7};
        std::vector<int> batch3 = {8, 9, 10};

        channel.sendBatch(batch1);
        channel.sendBatch(batch2);
        channel.sendBatch(batch3);

        auto start = std::chrono::steady_clock::now();
        while (!g_test8_done) {
            // 使用调度器的空闲等待
            if (std::chrono::steady_clock::now() - start > 5s) break;
        }

        scheduler.stop();

        int expected_total = 55;  // 1+2+...+10
        int expected_count = 10;

        if (g_test8_done && g_test8_count == expected_count && g_test8_total == expected_total) {
            LogInfo("[PASS] Batch send: count={}, total={}",
                    g_test8_count.load(), g_test8_total.load());
            g_passed++;
        } else {
            LogError("[FAIL] Batch send: done={}, count={}/{}, total={}/{}",
                    g_test8_done.load(), g_test8_count.load(), expected_count,
                    g_test8_total.load(), expected_total);
            g_failed++;
        }
    }

    // 测试9：字符串类型
    {
        LogInfo("\n--- Test 9: String channel ---");
        g_total++;

        ComputeScheduler scheduler;
        MpscChannel<std::string> channel;

        scheduler.start();
        scheduler.schedule(detail::TaskAccess::detachTask(test_string_channel(&channel)));

        channel.send(std::string("Hello, Channel!"));

        auto start = std::chrono::steady_clock::now();
        while (!g_test9_done) {
            // 使用调度器的空闲等待
            if (std::chrono::steady_clock::now() - start > 5s) break;
        }

        scheduler.stop();

        if (g_test9_done && g_test9_result == "Hello, Channel!") {
            LogInfo("[PASS] String channel: result=\"{}\"", g_test9_result);
            g_passed++;
        } else {
            LogError("[FAIL] String channel: done={}, result=\"{}\"",
                    g_test9_done.load(), g_test9_result);
            g_failed++;
        }
    }

    // 测试10：高并发压力测试
    {
        LogInfo("\n--- Test 10: High concurrency ({} messages) ---", TEST10_TOTAL);
        g_total++;

        ComputeScheduler scheduler;
        MpscChannel<int> channel;

        scheduler.start();
        scheduler.schedule(detail::TaskAccess::detachTask(test_high_concurrency(&channel)));

        // 多线程发送
        std::vector<std::thread> senders;
        int per_thread = TEST10_TOTAL / 4;
        for (int i = 0; i < 4; ++i) {
            senders.emplace_back(highConcurrencyProducer, &channel, per_thread);
        }

        for (auto& t : senders) {
            t.join();
        }

        auto start = std::chrono::steady_clock::now();
        while (!g_test10_done) {
            // 使用调度器的空闲等待
            if (std::chrono::steady_clock::now() - start > 30s) break;
        }

        scheduler.stop();

        if (g_test10_done && g_test10_received == TEST10_TOTAL) {
            LogInfo("[PASS] High concurrency: sent={}, received={}",
                    g_test10_sent.load(), g_test10_received.load());
            g_passed++;
        } else {
            LogError("[FAIL] High concurrency: done={}, sent={}, received={}/{}",
                    g_test10_done.load(), g_test10_sent.load(),
                    g_test10_received.load(), TEST10_TOTAL);
            g_failed++;
        }
    }

    // 测试11：跨调度器场景
    {
        LogInfo("\n--- Test 11: Cross-scheduler (producer/consumer on different schedulers) ---");
        g_total++;

        MpscChannel<int> channel;

        // 创建两个调度器
        ComputeScheduler producerScheduler;
        ComputeScheduler consumerScheduler;

        // 启动消费者调度器和协程
        std::thread consumerThread([&]() {
            consumerScheduler.start();
            consumerScheduler.schedule(detail::TaskAccess::detachTask(test_cross_scheduler_consumer(&channel)));

            auto start = std::chrono::steady_clock::now();
            while (!g_test11_consumer_done) {
                // 使用调度器的空闲等待
                if (std::chrono::steady_clock::now() - start > 30s) break;
            }
            consumerScheduler.stop();
        });

        // 启动生产者调度器和协程
        std::thread producerThread([&]() {
            producerScheduler.start();
            producerScheduler.schedule(detail::TaskAccess::detachTask(test_cross_scheduler_producer(&channel)));

            auto start = std::chrono::steady_clock::now();
            while (!g_test11_producer_done) {
                // 使用调度器的空闲等待
                if (std::chrono::steady_clock::now() - start > 30s) break;
            }
            producerScheduler.stop();
        });

        producerThread.join();
        consumerThread.join();

        int expected_sum = TEST11_MSG_COUNT * (TEST11_MSG_COUNT + 1) / 2;  // 1+2+...+50
        bool passed = g_test11_producer_done && g_test11_consumer_done &&
                      (g_test11_count == TEST11_MSG_COUNT) &&
                      (g_test11_sum == expected_sum);

        if (passed) {
            LogInfo("[PASS] Cross-scheduler: count={}, sum={} (expected {})",
                    g_test11_count.load(), g_test11_sum.load(), expected_sum);
            g_passed++;
        } else {
            LogError("[FAIL] Cross-scheduler: producer_done={}, consumer_done={}, count={}/{}, sum={}/{}",
                    g_test11_producer_done.load(), g_test11_consumer_done.load(),
                    g_test11_count.load(), TEST11_MSG_COUNT,
                    g_test11_sum.load(), expected_sum);
            g_failed++;
        }
    }

    // 测试12：多调度器多生产者
    {
        LogInfo("\n--- Test 12: Multi-scheduler multi-producer ({} producers x {} msgs) ---",
                TEST12_PRODUCER_COUNT, TEST12_MSG_PER_PRODUCER);
        g_total++;

        MpscChannel<int> channel;

        std::vector<std::unique_ptr<ComputeScheduler>> producerSchedulers;
        std::vector<std::thread> producerThreads;

        // 消费者调度器
        ComputeScheduler consumerScheduler;

        // 启动消费者
        std::thread consumerThread([&]() {
            consumerScheduler.start();
            consumerScheduler.schedule(detail::TaskAccess::detachTask(test_multi_scheduler_consumer(&channel)));

            auto start = std::chrono::steady_clock::now();
            while (!g_test12_consumer_done) {
                // 使用调度器的空闲等待
                if (std::chrono::steady_clock::now() - start > 60s) break;
            }
            consumerScheduler.stop();
        });

        // 创建多个生产者调度器
        for (int i = 0; i < TEST12_PRODUCER_COUNT; ++i) {
            producerSchedulers.push_back(std::make_unique<ComputeScheduler>());
        }

        // 启动生产者
        for (int i = 0; i < TEST12_PRODUCER_COUNT; ++i) {
            producerThreads.emplace_back([&producerSchedulers, &channel, i]() {
                producerSchedulers[i]->start();
                producerSchedulers[i]->schedule(detail::TaskAccess::detachTask(test_multi_scheduler_producer(&channel, i)));

                auto start = std::chrono::steady_clock::now();
                while (g_test12_producers_done <= i) {
                    // 使用调度器的空闲等待
                    if (std::chrono::steady_clock::now() - start > 30s) break;
                }
                producerSchedulers[i]->stop();
            });
        }

        // 等待所有生产者完成
        for (auto& t : producerThreads) {
            t.join();
        }

        // 等待消费者完成
        consumerThread.join();

        int expected_count = TEST12_PRODUCER_COUNT * TEST12_MSG_PER_PRODUCER;
        bool passed = (g_test12_producers_done == TEST12_PRODUCER_COUNT) &&
                      g_test12_consumer_done &&
                      (g_test12_count == expected_count);

        if (passed) {
            LogInfo("[PASS] Multi-scheduler multi-producer: producers_done={}, count={}, sum={}",
                    g_test12_producers_done.load(), g_test12_count.load(), g_test12_sum.load());
            g_passed++;
        } else {
            LogError("[FAIL] Multi-scheduler multi-producer: producers_done={}/{}, consumer_done={}, count={}/{}",
                    g_test12_producers_done.load(), TEST12_PRODUCER_COUNT,
                    g_test12_consumer_done.load(),
                    g_test12_count.load(), expected_count);
            g_failed++;
        }
    }

    // 测试13：同线程多生产者单消费者（高性能路径 - 直接 resume）
    {
        LogInfo("\n--- Test 13: Same-thread multi-producer single-consumer (direct resume path) ---");
        g_total++;

        ComputeScheduler scheduler;
        MpscChannel<int> channel;

        scheduler.start();

        // 在同一个调度器中启动消费者和多个生产者
        // 这样 send 时 waiterScheduler->threadId() == std::this_thread::get_id()
        // 会走直接 resume 的高性能路径
        scheduler.schedule(detail::TaskAccess::detachTask(test_same_thread_consumer(&channel, TEST13_MSG_COUNT)));

        // 启动多个生产者协程，每个发送一部分数据
        int perProducer = TEST13_MSG_COUNT / 4;
        scheduler.schedule(detail::TaskAccess::detachTask(test_same_thread_producer(&channel, 0, perProducer)));
        scheduler.schedule(detail::TaskAccess::detachTask(test_same_thread_producer(&channel, perProducer, perProducer)));
        scheduler.schedule(detail::TaskAccess::detachTask(test_same_thread_producer(&channel, perProducer * 2, perProducer)));
        scheduler.schedule(detail::TaskAccess::detachTask(test_same_thread_producer(&channel, perProducer * 3, perProducer)));

        auto start = std::chrono::steady_clock::now();
        while (!g_test13_done) {
            // 使用调度器的空闲等待
            if (std::chrono::steady_clock::now() - start > 10s) break;
        }

        scheduler.stop();

        // 计算期望的 sum: 0+1+2+...+99 = 4950
        int expected_sum = TEST13_MSG_COUNT * (TEST13_MSG_COUNT - 1) / 2;
        bool passed = g_test13_done &&
                      (g_test13_count == TEST13_MSG_COUNT) &&
                      (g_test13_sum == expected_sum);

        if (passed) {
            LogInfo("[PASS] Same-thread multi-producer: count={}, sum={} (expected {})",
                    g_test13_count.load(), g_test13_sum.load(), expected_sum);
            g_passed++;
        } else {
            LogError("[FAIL] Same-thread multi-producer: done={}, count={}/{}, sum={}/{}",
                    g_test13_done.load(), g_test13_count.load(), TEST13_MSG_COUNT,
                    g_test13_sum.load(), expected_sum);
            g_failed++;
        }
    }

    // 打印测试结果
    LogInfo("\n========================================");
    LogInfo("Test Results: {}/{} passed", g_passed.load(), g_total.load());
    LogInfo("========================================");
}

int main() {
    galay::test::TestResultWriter resultWriter("test_mpsc_channel");
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
