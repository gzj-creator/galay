/**
 * @file t15_unsafe.cc
 * @brief 用途：验证 `UnsafeChannel` 在单调度器协程通信场景下的正确性。
 * 关键覆盖点：协程生产消费、`co_yield` 协作、消息完整性和关闭行为。
 * 通过条件：消息收发结果符合预期，所有子测试通过并返回 0。
 */

#include <atomic>
#include <chrono>
#include <vector>
#include <galay/cpp/galay-kernel/concurrency/unsafe_channel.h>
#include <galay/cpp/galay-kernel/core/task.h>
#include "test/stdout_log.h"
#include "result_writer.h"

#ifdef USE_EPOLL
#include <galay/cpp/galay-kernel/core/epoll_scheduler.h>
using IOSchedulerType = galay::kernel::EpollScheduler;
#elif defined(USE_KQUEUE)
#include <galay/cpp/galay-kernel/core/kqueue_scheduler.h>
using IOSchedulerType = galay::kernel::KqueueScheduler;
#elif defined(USE_IOURING)
#include <galay/cpp/galay-kernel/core/uring_scheduler.h>
using IOSchedulerType = galay::kernel::IOUringScheduler;
#endif

using namespace galay::kernel;
using namespace std::chrono_literals;

std::atomic<int> g_passed{0};
std::atomic<int> g_total{0};

// ============================================================================
// 测试1：基本 send/recv
// ============================================================================
std::atomic<bool> g_test1_done{false};
int g_test1_received{0};

Task<void> test_basic_send_recv(UnsafeChannel<int>* channel) {
    auto value = co_await channel->recv();
    if (value && *value == 42) {
        g_test1_received = *value;
    }
    g_test1_done = true;
    co_return;
}

Task<void> test_basic_sender(UnsafeChannel<int>* channel) {
    channel->send(42);
    co_return;
}

// ============================================================================
// 测试2：多次 send/recv
// ============================================================================
int g_test2_sum{0};
std::atomic<bool> g_test2_done{false};
constexpr int TEST2_COUNT = 10;

Task<void> test_multiple_recv(UnsafeChannel<int>* channel) {
    for (int i = 0; i < TEST2_COUNT; ++i) {
        auto value = co_await channel->recv();
        if (value) {
            g_test2_sum += *value;
        }
    }
    g_test2_done = true;
    co_return;
}

Task<void> test_multiple_send(UnsafeChannel<int>* channel) {
    for (int i = 0; i < TEST2_COUNT; ++i) {
        channel->send(i + 1);
        co_yield true;
    }
    co_return;
}

// ============================================================================
// 测试3：批量发送/接收
// ============================================================================
int g_test3_total{0};
std::atomic<bool> g_test3_done{false};

Task<void> test_batch_recv(UnsafeChannel<int>* channel) {
    auto batch = co_await channel->recvBatch(100);
    if (batch) {
        for (int v : *batch) {
            g_test3_total += v;
        }
    }
    g_test3_done = true;
    co_return;
}

Task<void> test_batch_send(UnsafeChannel<int>* channel) {
    std::vector<int> data = {1, 2, 3, 4, 5};
    channel->sendBatch(data);
    co_return;
}

// ============================================================================
// 测试4：try_recv（非阻塞）
// ============================================================================
std::atomic<bool> g_test4_done{false};
int g_test4_value{0};

Task<void> test_try_recv(UnsafeChannel<int>* channel) {
    // 先尝试接收（应该为空）
    auto empty = channel->tryRecv();
    if (empty) {
        g_test4_done = true;
        co_return;  // 不应该到这里
    }

    // 等待数据
    auto value = co_await channel->recv();
    if (value) {
        g_test4_value = *value;
    }
    g_test4_done = true;
    co_return;
}

Task<void> test_try_recv_sender(UnsafeChannel<int>* channel) {
    co_yield true;  // 让消费者先执行
    channel->send(99);
    co_return;
}

// ============================================================================
// 测试5：同调度器多生产者单消费者
// ============================================================================
int g_test5_sum{0};
int g_test5_recv_count{0};
std::atomic<bool> g_test5_done{false};
constexpr int TEST5_PRODUCER_COUNT = 5;
constexpr int TEST5_MSG_PER_PRODUCER = 10;

Task<void> test_multi_producer_consumer(UnsafeChannel<int>* channel) {
    int expected = TEST5_PRODUCER_COUNT * TEST5_MSG_PER_PRODUCER;
    while (g_test5_recv_count < expected) {
        auto value = co_await channel->recv();
        if (value) {
            g_test5_sum += *value;
            g_test5_recv_count++;
        }
    }
    g_test5_done = true;
    co_return;
}

Task<void> test_producer(UnsafeChannel<int>* channel, int id) {
    for (int i = 0; i < TEST5_MSG_PER_PRODUCER; ++i) {
        channel->send(id * 100 + i);
        co_yield true;
    }
    co_return;
}

// ============================================================================
// 测试6：空通道等待
// ============================================================================
std::atomic<bool> g_test6_waiting{false};
std::atomic<bool> g_test6_received{false};
std::atomic<bool> g_test6_done{false};

Task<void> test_empty_channel_wait(UnsafeChannel<int>* channel) {
    g_test6_waiting = true;
    auto value = co_await channel->recv();
    g_test6_received = value.has_value();
    g_test6_done = true;
    co_return;
}

Task<void> test_delayed_send(UnsafeChannel<int>* channel) {
    // 等待消费者开始等待
    while (!g_test6_waiting) {
        co_yield true;
    }
    channel->send(123);
    co_return;
}

// ============================================================================
// 测试7：size() 和 empty()
// ============================================================================
std::atomic<bool> g_test7_done{false};

Task<void> test_size_and_empty(UnsafeChannel<int>* channel) {
    // 消费所有数据
    while (!channel->empty()) {
        co_await channel->recv();
    }
    g_test7_done = true;
    co_return;
}

// ============================================================================
// 测试8：批量发送多次
// ============================================================================
int g_test8_total{0};
int g_test8_count{0};
std::atomic<bool> g_test8_done{false};

Task<void> test_batch_recv_multiple(UnsafeChannel<int>* channel) {
    // 接收所有数据
    for (int i = 0; i < 3; ++i) {
        auto batch = co_await channel->recvBatch(100);
        if (batch) {
            g_test8_count += batch->size();
            for (int v : *batch) {
                g_test8_total += v;
            }
        }
    }
    g_test8_done = true;
    co_return;
}

Task<void> test_batch_send_multiple(UnsafeChannel<int>* channel) {
    std::vector<int> batch1 = {1, 2, 3};
    std::vector<int> batch2 = {4, 5, 6, 7};
    std::vector<int> batch3 = {8, 9, 10};

    channel->sendBatch(batch1);
    co_yield true;
    channel->sendBatch(batch2);
    co_yield true;
    channel->sendBatch(batch3);
    co_return;
}

// ============================================================================
// 测试9：字符串类型
// ============================================================================
std::atomic<bool> g_test9_done{false};
std::string g_test9_result;

Task<void> test_string_recv(UnsafeChannel<std::string>* channel) {
    auto value = co_await channel->recv();
    if (value) {
        g_test9_result = *value;
    }
    g_test9_done = true;
    co_return;
}

Task<void> test_string_send(UnsafeChannel<std::string>* channel) {
    channel->send(std::string("Hello, UnsafeChannel!"));
    co_return;
}

// ============================================================================
// 测试10：recvBatched - 达到 limit 时唤醒
// ============================================================================
std::atomic<bool> g_test10_done{false};
int g_test10_received_count{0};
int g_test10_sum{0};
constexpr int TEST10_LIMIT = 10;

Task<void> test_recv_batched_limit(UnsafeChannel<int>* channel) {
    auto result = co_await channel->recvBatched(TEST10_LIMIT);
    if (result) {
        g_test10_received_count = result->size();
        for (int v : *result) {
            g_test10_sum += v;
        }
    }
    g_test10_done = true;
    co_return;
}

Task<void> test_recv_batched_sender(UnsafeChannel<int>* channel) {
    // 逐个发送，当达到 limit 时应该唤醒消费者
    for (int i = 1; i <= TEST10_LIMIT; ++i) {
        channel->send(i);
        co_yield true;
    }
    co_return;
}

// ============================================================================
// 测试11：recvBatched - 超时后用 tryRecvBatch 获取部分数据
// ============================================================================
std::atomic<bool> g_test11_done{false};
int g_test11_received_count{0};
int g_test11_sum{0};

Task<void> test_recv_batched_timeout(UnsafeChannel<int>* channel) {
    // 等待 100 条或 100ms 超时
    auto result = co_await channel->recvBatched(100).timeout(100ms);
    if (!result) {
        // 超时了，用 tryRecvBatch 获取队列中的部分数据
        auto partial = channel->tryRecvBatch();
        if (partial) {
            g_test11_received_count = partial->size();
            for (int v : *partial) {
                g_test11_sum += v;
            }
        }
    }
    g_test11_done = true;
    co_return;
}

Task<void> test_recv_batched_timeout_sender(UnsafeChannel<int>* channel) {
    // 只发送 5 条，不足 100 条，会超时
    for (int i = 1; i <= 5; ++i) {
        channel->send(i);
        co_yield true;
    }
    co_return;
}

// ============================================================================
// 测试12：recvBatched - 超时且无数据
// ============================================================================
std::atomic<bool> g_test12_done{false};
bool g_test12_got_error{false};

Task<void> test_recv_batched_timeout_empty(UnsafeChannel<int>* channel) {
    // 等待 100 条或 50ms 超时，但没有数据发送
    auto result = co_await channel->recvBatched(100).timeout(50ms);
    if (!result) {
        g_test12_got_error = true;
    }
    g_test12_done = true;
    co_return;
}

// ============================================================================
// 测试13：recvBatched - 队列已有足够数据
// ============================================================================
std::atomic<bool> g_test13_done{false};
int g_test13_received_count{0};

Task<void> test_recv_batched_ready(UnsafeChannel<int>* channel) {
    // 队列已有 20 条数据，limit 是 10，应该立即返回所有数据
    auto result = co_await channel->recvBatched(10);
    if (result) {
        g_test13_received_count = result->size();
    }
    g_test13_done = true;
    co_return;
}

// ============================================================================
// 测试14：recvBatched - 与普通 recv 混用
// ============================================================================
std::atomic<bool> g_test14_done{false};
int g_test14_batched_count{0};
int g_test14_single_value{0};

Task<void> test_recv_batched_mixed(UnsafeChannel<int>* channel) {
    // 先用 recvBatched 接收
    auto batch = co_await channel->recvBatched(5);
    if (batch) {
        g_test14_batched_count = batch->size();
    }

    // 再用普通 recv 接收
    auto single = co_await channel->recv();
    if (single) {
        g_test14_single_value = *single;
    }

    g_test14_done = true;
    co_return;
}

Task<void> test_recv_batched_mixed_sender(UnsafeChannel<int>* channel) {
    // 发送 5 条触发 recvBatched
    for (int i = 1; i <= 5; ++i) {
        channel->send(i);
        co_yield true;
    }
    // 再发送 1 条触发 recv
    channel->send(99);
    co_return;
}

// ============================================================================
// 测试15：高并发（同调度器内）
// ============================================================================
int g_test15_received{0};
std::atomic<bool> g_test15_done{false};
constexpr int TEST15_TOTAL = 1000;

Task<void> test_high_concurrency_consumer(UnsafeChannel<int>* channel) {
    while (g_test15_received < TEST15_TOTAL) {
        auto value = co_await channel->recv();
        if (value) {
            g_test15_received++;
        }
    }
    g_test15_done = true;
    co_return;
}

Task<void> test_high_concurrency_producer(UnsafeChannel<int>* channel, int start, int count) {
    for (int i = 0; i < count; ++i) {
        channel->send(start + i);
        if (i % 10 == 0) {
            co_yield true;
        }
    }
    co_return;
}

// ============================================================================
// 测试16：send immediately=true 立即唤醒
// ============================================================================
std::atomic<bool> g_test16_done{false};
int g_test16_received_count{0};
int g_test16_sum{0};

Task<void> test_send_immediately_consumer(UnsafeChannel<int>* channel) {
    // 使用 recvBatched(100)，正常情况下需要等到 100 条
    auto result = co_await channel->recvBatched(100).timeout(100ms);
    if (result) {
        g_test16_received_count = result->size();
        for (int v : *result) {
            g_test16_sum += v;
        }
    }
    g_test16_done = true;
    co_return;
}

Task<void> test_send_immediately_sender(UnsafeChannel<int>* channel) {
    // 只发送 5 条，但最后一条使用 immediately=true
    for (int i = 1; i <= 4; ++i) {
        channel->send(i, false);  // 不立即唤醒
        co_yield true;
    }
    // 最后一条立即唤醒，即使不足 100 条
    channel->send(5, true);
    co_return;
}

// ============================================================================
// 测试17：sendBatch immediately=true 立即唤醒
// ============================================================================
std::atomic<bool> g_test17_done{false};
int g_test17_received_count{0};

Task<void> test_send_batch_immediately_consumer(UnsafeChannel<int>* channel) {
    auto result = co_await channel->recvBatched(100).timeout(100ms);
    if (result) {
        g_test17_received_count = result->size();
    }
    g_test17_done = true;
    co_return;
}

Task<void> test_send_batch_immediately_sender(UnsafeChannel<int>* channel) {
    std::vector<int> batch = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    // 批量发送 10 条，使用 immediately=true 立即唤醒
    channel->sendBatch(batch, true);
    co_return;
}

// ============================================================================
// 测试18：immediately 参数对比测试
// ============================================================================
std::atomic<bool> g_test18_done{false};
int g_test18_received_count{0};

Task<void> test_immediately_compare_consumer(UnsafeChannel<int>* channel) {
    // 使用普通 recv，不使用 recvBatched
    int count = 0;
    for (int i = 0; i < 3; ++i) {
        auto result = co_await channel->recv();
        if (result) {
            count++;
        }
    }
    g_test18_received_count = count;
    g_test18_done = true;
    co_return;
}

Task<void> test_immediately_compare_sender(UnsafeChannel<int>* channel) {
    // 发送 3 条数据，第 3 条使用 immediately=true
    channel->send(1, false);  // 不立即唤醒
    co_yield true;
    channel->send(2, false);  // 不立即唤醒
    co_yield true;
    channel->send(3, true);   // 立即唤醒，消费者会被唤醒并取走所有 3 条
    co_return;
}

// ============================================================================
// 测试19：超时后使用 tryRecvBatch 获取部分数据
// ============================================================================
std::atomic<bool> g_test19_done{false};
int g_test19_received_count{0};
int g_test19_sum{0};

Task<void> test_timeout_auto_return(UnsafeChannel<int>* channel) {
    // 尝试等待 100 条数据，但只会收到 5 条，超时后返回错误
    auto result = co_await channel->recvBatched(100).timeout(50ms);

    if (!result) {
        // 超时了，使用 tryRecvBatch 获取队列中的部分数据
        auto partial = channel->tryRecvBatch();
        if (partial) {
            g_test19_received_count = partial->size();
            for (int v : *partial) {
                g_test19_sum += v;
            }
        }
    }

    g_test19_done = true;
    co_return;
}

Task<void> test_timeout_auto_return_sender(UnsafeChannel<int>* channel) {
    // 一次性发送 5 条数据，不足 100 条，immediately=false 不会唤醒
    for (int i = 1; i <= 5; ++i) {
        channel->send(i, false);
    }
    co_return;
}

// ============================================================================
// 主函数
// ============================================================================
void runTests() {
    LogInfo("========================================");
    LogInfo("UnsafeChannel Unit Tests");
    LogInfo("========================================");

#if defined(USE_EPOLL) || defined(USE_KQUEUE) || defined(USE_IOURING)

    // 测试1：基本 send/recv
    {
        LogInfo("\n--- Test 1: Basic send/recv ---");
        g_total++;

        IOSchedulerType scheduler;
        UnsafeChannel<int> channel;

        scheduler.start();
        scheduler.schedule(detail::TaskAccess::detachTask(test_basic_send_recv(&channel)));
        scheduler.schedule(detail::TaskAccess::detachTask(test_basic_sender(&channel)));

        auto start = std::chrono::steady_clock::now();
        while (!g_test1_done) {
            // 使用调度器的空闲等待
            if (std::chrono::steady_clock::now() - start > 5s) break;
        }

        scheduler.stop();

        if (g_test1_done && g_test1_received == 42) {
            LogInfo("[PASS] Basic send/recv: received={}", g_test1_received);
            g_passed++;
        } else {
            LogError("[FAIL] Basic send/recv: done={}, received={}",
                    g_test1_done.load(), g_test1_received);
        }
    }

    // 测试2：多次 send/recv
    {
        LogInfo("\n--- Test 2: Multiple send/recv ({} messages) ---", TEST2_COUNT);
        g_total++;

        IOSchedulerType scheduler;
        UnsafeChannel<int> channel;

        scheduler.start();
        scheduler.schedule(detail::TaskAccess::detachTask(test_multiple_recv(&channel)));
        scheduler.schedule(detail::TaskAccess::detachTask(test_multiple_send(&channel)));

        auto start = std::chrono::steady_clock::now();
        while (!g_test2_done) {
            // 使用调度器的空闲等待
            if (std::chrono::steady_clock::now() - start > 10s) break;
        }

        scheduler.stop();

        int expected_sum = TEST2_COUNT * (TEST2_COUNT + 1) / 2;  // 1+2+...+10 = 55
        if (g_test2_done && g_test2_sum == expected_sum) {
            LogInfo("[PASS] Multiple send/recv: sum={} (expected {})",
                    g_test2_sum, expected_sum);
            g_passed++;
        } else {
            LogError("[FAIL] Multiple send/recv: done={}, sum={} (expected {})",
                    g_test2_done.load(), g_test2_sum, expected_sum);
        }
    }

    // 测试3：批量发送/接收
    {
        LogInfo("\n--- Test 3: Batch send/recv ---");
        g_total++;

        IOSchedulerType scheduler;
        UnsafeChannel<int> channel;

        scheduler.start();
        scheduler.schedule(detail::TaskAccess::detachTask(test_batch_recv(&channel)));
        scheduler.schedule(detail::TaskAccess::detachTask(test_batch_send(&channel)));

        auto start = std::chrono::steady_clock::now();
        while (!g_test3_done) {
            // 使用调度器的空闲等待
            if (std::chrono::steady_clock::now() - start > 5s) break;
        }

        scheduler.stop();

        int expected = 15;  // 1+2+3+4+5
        if (g_test3_done && g_test3_total == expected) {
            LogInfo("[PASS] Batch send/recv: total={} (expected {})",
                    g_test3_total, expected);
            g_passed++;
        } else {
            LogError("[FAIL] Batch send/recv: done={}, total={} (expected {})",
                    g_test3_done.load(), g_test3_total, expected);
        }
    }

    // 测试4：try_recv
    {
        LogInfo("\n--- Test 4: try_recv (non-blocking) ---");
        g_total++;

        IOSchedulerType scheduler;
        UnsafeChannel<int> channel;

        scheduler.start();
        scheduler.schedule(detail::TaskAccess::detachTask(test_try_recv(&channel)));
        scheduler.schedule(detail::TaskAccess::detachTask(test_try_recv_sender(&channel)));

        auto start = std::chrono::steady_clock::now();
        while (!g_test4_done) {
            // 使用调度器的空闲等待
            if (std::chrono::steady_clock::now() - start > 5s) break;
        }

        scheduler.stop();

        if (g_test4_done && g_test4_value == 99) {
            LogInfo("[PASS] try_recv: value={}", g_test4_value);
            g_passed++;
        } else {
            LogError("[FAIL] try_recv: done={}, value={}",
                    g_test4_done.load(), g_test4_value);
        }
    }

    // 测试5：同调度器多生产者
    {
        LogInfo("\n--- Test 5: Same-scheduler multi-producer ({} producers x {} messages) ---",
                TEST5_PRODUCER_COUNT, TEST5_MSG_PER_PRODUCER);
        g_total++;

        IOSchedulerType scheduler;
        UnsafeChannel<int> channel;

        scheduler.start();
        scheduler.schedule(detail::TaskAccess::detachTask(test_multi_producer_consumer(&channel)));

        // 启动多个生产者协程（同一调度器内）
        for (int i = 0; i < TEST5_PRODUCER_COUNT; ++i) {
            scheduler.schedule(detail::TaskAccess::detachTask(test_producer(&channel, i)));
        }

        auto start = std::chrono::steady_clock::now();
        while (!g_test5_done) {
            // 使用调度器的空闲等待
            if (std::chrono::steady_clock::now() - start > 10s) break;
        }

        scheduler.stop();

        int expected_count = TEST5_PRODUCER_COUNT * TEST5_MSG_PER_PRODUCER;
        if (g_test5_done && g_test5_recv_count == expected_count) {
            LogInfo("[PASS] Same-scheduler multi-producer: received={}, sum={}",
                    g_test5_recv_count, g_test5_sum);
            g_passed++;
        } else {
            LogError("[FAIL] Same-scheduler multi-producer: done={}, received={}/{}",
                    g_test5_done.load(), g_test5_recv_count, expected_count);
        }
    }

    // 测试6：空通道等待
    {
        LogInfo("\n--- Test 6: Empty channel wait ---");
        g_total++;

        IOSchedulerType scheduler;
        UnsafeChannel<int> channel;

        scheduler.start();
        scheduler.schedule(detail::TaskAccess::detachTask(test_empty_channel_wait(&channel)));
        scheduler.schedule(detail::TaskAccess::detachTask(test_delayed_send(&channel)));

        auto start = std::chrono::steady_clock::now();
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
        }
    }

    // 测试7：size() 和 empty()
    {
        LogInfo("\n--- Test 7: size() and empty() ---");
        g_total++;

        IOSchedulerType scheduler;
        UnsafeChannel<int> channel;

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
        }
    }

    // 测试8：批量发送多次
    {
        LogInfo("\n--- Test 8: Batch send multiple ---");
        g_total++;

        IOSchedulerType scheduler;
        UnsafeChannel<int> channel;

        scheduler.start();
        scheduler.schedule(detail::TaskAccess::detachTask(test_batch_recv_multiple(&channel)));
        scheduler.schedule(detail::TaskAccess::detachTask(test_batch_send_multiple(&channel)));

        auto start = std::chrono::steady_clock::now();
        while (!g_test8_done) {
            // 使用调度器的空闲等待
            if (std::chrono::steady_clock::now() - start > 5s) break;
        }

        scheduler.stop();

        int expected_total = 55;  // 1+2+...+10
        int expected_count = 10;

        if (g_test8_done && g_test8_count == expected_count && g_test8_total == expected_total) {
            LogInfo("[PASS] Batch send multiple: count={}, total={}",
                    g_test8_count, g_test8_total);
            g_passed++;
        } else {
            LogError("[FAIL] Batch send multiple: done={}, count={}/{}, total={}/{}",
                    g_test8_done.load(), g_test8_count, expected_count,
                    g_test8_total, expected_total);
        }
    }

    // 测试9：字符串类型
    {
        LogInfo("\n--- Test 9: String channel ---");
        g_total++;

        IOSchedulerType scheduler;
        UnsafeChannel<std::string> channel;

        scheduler.start();
        scheduler.schedule(detail::TaskAccess::detachTask(test_string_recv(&channel)));
        scheduler.schedule(detail::TaskAccess::detachTask(test_string_send(&channel)));

        auto start = std::chrono::steady_clock::now();
        while (!g_test9_done) {
            // 使用调度器的空闲等待
            if (std::chrono::steady_clock::now() - start > 5s) break;
        }

        scheduler.stop();

        if (g_test9_done && g_test9_result == "Hello, UnsafeChannel!") {
            LogInfo("[PASS] String channel: result=\"{}\"", g_test9_result);
            g_passed++;
        } else {
            LogError("[FAIL] String channel: done={}, result=\"{}\"",
                    g_test9_done.load(), g_test9_result);
        }
    }

    // 测试10：recvBatched - 达到 limit 时唤醒
    {
        LogInfo("\n--- Test 10: recvBatched - wake on limit ({} items) ---", TEST10_LIMIT);
        g_total++;

        IOSchedulerType scheduler;
        UnsafeChannel<int> channel;

        scheduler.start();
        scheduler.schedule(detail::TaskAccess::detachTask(test_recv_batched_limit(&channel)));
        scheduler.schedule(detail::TaskAccess::detachTask(test_recv_batched_sender(&channel)));

        auto start = std::chrono::steady_clock::now();
        while (!g_test10_done) {
            if (std::chrono::steady_clock::now() - start > 5s) break;
        }

        scheduler.stop();

        int expected_sum = TEST10_LIMIT * (TEST10_LIMIT + 1) / 2;  // 1+2+...+10 = 55
        if (g_test10_done && g_test10_received_count == TEST10_LIMIT && g_test10_sum == expected_sum) {
            LogInfo("[PASS] recvBatched limit: count={}, sum={}",
                    g_test10_received_count, g_test10_sum);
            g_passed++;
        } else {
            LogError("[FAIL] recvBatched limit: done={}, count={}/{}, sum={}/{}",
                    g_test10_done.load(), g_test10_received_count, TEST10_LIMIT,
                    g_test10_sum, expected_sum);
        }
    }

    // 测试11：recvBatched - 超时返回部分数据
    {
        LogInfo("\n--- Test 11: recvBatched - timeout with partial data ---");
        g_total++;

        IOSchedulerType scheduler;
        UnsafeChannel<int> channel;

        scheduler.start();
        scheduler.schedule(detail::TaskAccess::detachTask(test_recv_batched_timeout(&channel)));
        scheduler.schedule(detail::TaskAccess::detachTask(test_recv_batched_timeout_sender(&channel)));

        auto start = std::chrono::steady_clock::now();
        while (!g_test11_done) {
            if (std::chrono::steady_clock::now() - start > 5s) break;
        }

        scheduler.stop();

        int expected_sum = 15;  // 1+2+3+4+5
        if (g_test11_done && g_test11_received_count == 5 && g_test11_sum == expected_sum) {
            LogInfo("[PASS] recvBatched timeout partial: count={}, sum={}",
                    g_test11_received_count, g_test11_sum);
            g_passed++;
        } else {
            LogError("[FAIL] recvBatched timeout partial: done={}, count={}, sum={}",
                    g_test11_done.load(), g_test11_received_count, g_test11_sum);
        }
    }

    // 测试12：recvBatched - 超时且无数据
    {
        LogInfo("\n--- Test 12: recvBatched - timeout with no data ---");
        g_total++;

        IOSchedulerType scheduler;
        UnsafeChannel<int> channel;

        scheduler.start();
        scheduler.schedule(detail::TaskAccess::detachTask(test_recv_batched_timeout_empty(&channel)));

        auto start = std::chrono::steady_clock::now();
        while (!g_test12_done) {
            if (std::chrono::steady_clock::now() - start > 5s) break;
        }

        scheduler.stop();

        if (g_test12_done && g_test12_got_error) {
            LogInfo("[PASS] recvBatched timeout empty: got timeout error");
            g_passed++;
        } else {
            LogError("[FAIL] recvBatched timeout empty: done={}, got_error={}",
                    g_test12_done.load(), g_test12_got_error);
        }
    }

    // 测试13：recvBatched - 队列已有足够数据
    {
        LogInfo("\n--- Test 13: recvBatched - queue already has enough data ---");
        g_total++;

        IOSchedulerType scheduler;
        UnsafeChannel<int> channel;

        // 预先发送 20 条数据
        for (int i = 1; i <= 20; ++i) {
            channel.send(i);
        }

        scheduler.start();
        scheduler.schedule(detail::TaskAccess::detachTask(test_recv_batched_ready(&channel)));

        auto start = std::chrono::steady_clock::now();
        while (!g_test13_done) {
            if (std::chrono::steady_clock::now() - start > 5s) break;
        }

        scheduler.stop();

        // 应该立即返回所有 20 条数据（不等待 limit）
        if (g_test13_done && g_test13_received_count == 20) {
            LogInfo("[PASS] recvBatched ready: count={}", g_test13_received_count);
            g_passed++;
        } else {
            LogError("[FAIL] recvBatched ready: done={}, count={}",
                    g_test13_done.load(), g_test13_received_count);
        }
    }

    // 测试14：recvBatched - 与普通 recv 混用
    {
        LogInfo("\n--- Test 14: recvBatched - mixed with recv ---");
        g_total++;

        IOSchedulerType scheduler;
        UnsafeChannel<int> channel;

        scheduler.start();
        scheduler.schedule(detail::TaskAccess::detachTask(test_recv_batched_mixed(&channel)));
        scheduler.schedule(detail::TaskAccess::detachTask(test_recv_batched_mixed_sender(&channel)));

        auto start = std::chrono::steady_clock::now();
        while (!g_test14_done) {
            if (std::chrono::steady_clock::now() - start > 5s) break;
        }

        scheduler.stop();

        if (g_test14_done && g_test14_batched_count == 5 && g_test14_single_value == 99) {
            LogInfo("[PASS] recvBatched mixed: batched_count={}, single_value={}",
                    g_test14_batched_count, g_test14_single_value);
            g_passed++;
        } else {
            LogError("[FAIL] recvBatched mixed: done={}, batched_count={}, single_value={}",
                    g_test14_done.load(), g_test14_batched_count, g_test14_single_value);
        }
    }

    // 测试15：高并发（同调度器内）
    {
        LogInfo("\n--- Test 15: High concurrency ({} messages) ---", TEST15_TOTAL);
        g_total++;

        IOSchedulerType scheduler;
        UnsafeChannel<int> channel;

        scheduler.start();
        scheduler.schedule(detail::TaskAccess::detachTask(test_high_concurrency_consumer(&channel)));

        // 启动多个生产者协程
        int per_producer = TEST15_TOTAL / 4;
        for (int i = 0; i < 4; ++i) {
            scheduler.schedule(detail::TaskAccess::detachTask(test_high_concurrency_producer(&channel, i * per_producer, per_producer)));
        }

        auto start = std::chrono::steady_clock::now();
        while (!g_test15_done) {
            // 使用调度器的空闲等待
            if (std::chrono::steady_clock::now() - start > 30s) break;
        }

        scheduler.stop();

        if (g_test15_done && g_test15_received == TEST15_TOTAL) {
            LogInfo("[PASS] High concurrency: received={}",
                    g_test15_received);
            g_passed++;
        } else {
            LogError("[FAIL] High concurrency: done={}, received={}/{}",
                    g_test15_done.load(), g_test15_received, TEST15_TOTAL);
        }
    }

    // 测试16：send immediately=true 立即唤醒
    {
        LogInfo("\n--- Test 16: send immediately=true ---");
        g_total++;

        IOSchedulerType scheduler;
        UnsafeChannel<int> channel;

        scheduler.start();
        scheduler.schedule(detail::TaskAccess::detachTask(test_send_immediately_consumer(&channel)));
        scheduler.schedule(detail::TaskAccess::detachTask(test_send_immediately_sender(&channel)));

        auto start = std::chrono::steady_clock::now();
        while (!g_test16_done) {
            if (std::chrono::steady_clock::now() - start > 5s) break;
        }

        scheduler.stop();

        int expected_sum = 15;  // 1+2+3+4+5
        if (g_test16_done && g_test16_received_count == 5 && g_test16_sum == expected_sum) {
            LogInfo("[PASS] send immediately: count={}, sum={}",
                    g_test16_received_count, g_test16_sum);
            g_passed++;
        } else {
            LogError("[FAIL] send immediately: done={}, count={}, sum={}",
                    g_test16_done.load(), g_test16_received_count, g_test16_sum);
        }
    }

    // 测试17：sendBatch immediately=true 立即唤醒
    {
        LogInfo("\n--- Test 17: sendBatch immediately=true ---");
        g_total++;

        IOSchedulerType scheduler;
        UnsafeChannel<int> channel;

        scheduler.start();
        scheduler.schedule(detail::TaskAccess::detachTask(test_send_batch_immediately_consumer(&channel)));
        scheduler.schedule(detail::TaskAccess::detachTask(test_send_batch_immediately_sender(&channel)));

        auto start = std::chrono::steady_clock::now();
        while (!g_test17_done) {
            if (std::chrono::steady_clock::now() - start > 5s) break;
        }

        scheduler.stop();

        if (g_test17_done && g_test17_received_count == 10) {
            LogInfo("[PASS] sendBatch immediately: count={}", g_test17_received_count);
            g_passed++;
        } else {
            LogError("[FAIL] sendBatch immediately: done={}, count={}",
                    g_test17_done.load(), g_test17_received_count);
        }
    }

    // 测试18：immediately 参数对比测试
    {
        LogInfo("\n--- Test 18: immediately parameter comparison ---");
        g_total++;

        IOSchedulerType scheduler;
        UnsafeChannel<int> channel;

        scheduler.start();
        scheduler.schedule(detail::TaskAccess::detachTask(test_immediately_compare_consumer(&channel)));
        scheduler.schedule(detail::TaskAccess::detachTask(test_immediately_compare_sender(&channel)));

        auto start = std::chrono::steady_clock::now();
        while (!g_test18_done) {
            if (std::chrono::steady_clock::now() - start > 5s) break;
        }

        scheduler.stop();

        if (g_test18_done && g_test18_received_count == 3) {
            LogInfo("[PASS] immediately parameter: received {} items", g_test18_received_count);
            g_passed++;
        } else {
            LogError("[FAIL] immediately parameter: done={}, count={}",
                    g_test18_done.load(), g_test18_received_count);
        }
    }

    // 测试19：超时后使用 tryRecvBatch 获取部分数据
    {
        LogInfo("\n--- Test 19: timeout then tryRecvBatch for partial data ---");
        g_total++;

        IOSchedulerType scheduler;
        UnsafeChannel<int> channel;

        scheduler.start();
        scheduler.schedule(detail::TaskAccess::detachTask(test_timeout_auto_return(&channel)));
        scheduler.schedule(detail::TaskAccess::detachTask(test_timeout_auto_return_sender(&channel)));

        auto start = std::chrono::steady_clock::now();
        while (!g_test19_done) {
            if (std::chrono::steady_clock::now() - start > 5s) break;
        }

        scheduler.stop();

        int expected_sum = 15;  // 1+2+3+4+5
        if (g_test19_done && g_test19_received_count == 5 && g_test19_sum == expected_sum) {
            LogInfo("[PASS] timeout then tryRecvBatch: count={}, sum={}",
                    g_test19_received_count, g_test19_sum);
            g_passed++;
        } else {
            LogError("[FAIL] timeout then tryRecvBatch: done={}, count={}, sum={}",
                    g_test19_done.load(), g_test19_received_count, g_test19_sum);
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
    galay::test::TestResultWriter resultWriter("test_unsafe_channel");
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
