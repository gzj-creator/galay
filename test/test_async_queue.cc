#include "galay/kernel/runtime/Runtime.h"
#include "galay/kernel/coroutine/CoSchedulerHandle.hpp"
#include "galay/kernel/async/AsyncFactory.h"
#include "galay/kernel/coroutine/AsyncQueue.h"
#include "galay/common/Error.h"
#include <cstdio>
#include <iostream>
#include <chrono>
#include <vector>
#include <set>
#include <mutex>

using namespace galay;
using namespace std::chrono_literals;

// ==================== 测试数据量配置 ====================
// 修改这些宏来调整测试数据量

/** Test 1: 单生产者单消费者 (单线程) - 推荐用于基准测试 */
#ifndef ST_TEST1_ITEMS
#define ST_TEST1_ITEMS 100000
#endif

/** Test 2: 多生产者单消费者 (单线程) - 展示多生产者行为 */
#ifndef ST_TEST2_PRODUCER_COUNT
#define ST_TEST2_PRODUCER_COUNT 3
#endif
#ifndef ST_TEST2_ITEMS_PER_PRODUCER
#define ST_TEST2_ITEMS_PER_PRODUCER 10000
#endif

/** Test 3: 多生产者单消费者 (多线程) - 高压力并发测试 */
#ifndef MT_TEST3_PRODUCER_COUNT
#define MT_TEST3_PRODUCER_COUNT 4
#endif
#ifndef MT_TEST3_ITEMS_PER_PRODUCER
#define MT_TEST3_ITEMS_PER_PRODUCER 50000
#endif

// ==================== 数据验证辅助 ====================

// 全局 mutex 用于保护共享数据
std::mutex g_test_data_mutex;

/**
 * @brief 测试结果容器
 */
struct TestResult {
    std::string testName;
    int producedItems = 0;          // 总生产数
    int consumedItems = 0;          // 总消费数
    std::set<int> consumedValues;   // 消费的值集合（用于验证）
    std::map<int, int> valueCounts; // 每个值被消费的次数
    bool success = false;           // 是否成功
    std::string errorMsg;           // 错误信息
    int minProducerId = INT_MAX;    // 最小生产者ID
    int maxProducerId = 0;          // 最大生产者ID

    bool verify() const {
        return producedItems == consumedItems &&
               consumedValues.size() == static_cast<size_t>(consumedItems);
    }

    void recordConsumedItem(int value) {
        std::lock_guard<std::mutex> lock(g_test_data_mutex);
        consumedItems++;
        valueCounts[value]++;
        if (consumedValues.count(value) > 0) {
            // Duplicate detected - print warning
            // std::cerr << "[DUP] Value " << value << " consumed again!\n";
        }
        consumedValues.insert(value);

        // Track producer ID range
        int producerId = value / 1000;
        minProducerId = std::min(minProducerId, producerId);
        maxProducerId = std::max(maxProducerId, producerId);
    }

    void print() const {
        std::lock_guard<std::mutex> lock(g_test_data_mutex);
        std::cout << "\n[TestResult] " << testName << ": "
                  << (verify() ? "✅ PASS" : "❌ FAIL")
                  << " (produced: " << producedItems
                  << ", consumed: " << consumedItems
                  << ", unique: " << consumedValues.size() << ")" << std::endl;
        if (!errorMsg.empty()) {
            std::cout << "  Error: " << errorMsg << std::endl;
        }
        if (!verify()) {
            std::cout << "  Duplicates: " << (consumedItems - consumedValues.size()) << std::endl;
        }

        // Check for values with high duplicate counts
        int maxCount = 0;
        for (const auto& p : valueCounts) {
            maxCount = std::max(maxCount, p.second);
        }
        if (maxCount > 1) {
            std::cout << "  Max repeats for single value: " << maxCount << "x" << std::endl;
            int count = 0;
            for (const auto& p : valueCounts) {
                if (p.second > 1 && count < 3) {
                    std::cout << "    Value " << p.first << " repeated " << p.second << " times" << std::endl;
                    count++;
                }
            }
        }
    }
};

// 全局结果收集器
std::vector<std::shared_ptr<TestResult>> g_test_results;
std::vector<std::shared_ptr<TestResult>> g_all_test_results;  // 保存所有测试结果
std::mutex g_results_mutex;

void recordTestResult(std::shared_ptr<TestResult> result) {
    std::lock_guard<std::mutex> lock(g_results_mutex);
    g_test_results.push_back(result);
}

// ==================== 单线程协程测试 ====================

// 生产者协程 - 单线程版
Coroutine<nil> single_thread_producer(
    CoSchedulerHandle handle,
    std::shared_ptr<AsyncQueue<int>> queue,
    int producerId,
    int itemCount) {
    for (int i = 0; i < itemCount; ++i) {
        int value = producerId * 1000 + i;
        queue->enqueue(value);
    }

    co_return nil();
}

// 消费者协程 - 单线程版
Coroutine<nil> single_thread_consumer(
    CoSchedulerHandle handle,
    std::shared_ptr<AsyncQueue<int>> queue,
    int consumerId,
    int itemCount,
    std::shared_ptr<TestResult> result) {
    for (int i = 0; i < itemCount; ++i) {
        int value = co_await queue->waitDequeue();
        result->recordConsumedItem(value);
    }

    co_return nil();
}

// ==================== 多线程协程测试 ====================

/**
 * @brief 多线程生产者 - 在不同的调度器线程中运行
 * 模拟多个线程中的生产者协程
 */
Coroutine<nil> multi_thread_producer(
    CoSchedulerHandle handle,
    std::shared_ptr<AsyncQueue<int>> queue,
    int producerId,
    int itemCount) {
    for (int i = 0; i < itemCount; ++i) {
        int value = producerId * 1000 + i;
        queue->enqueue(value);
    }

    co_return nil();
}

/**
 * @brief 多线程消费者 - 在不同的调度器线程中运行
 */
Coroutine<nil> multi_thread_consumer(
    CoSchedulerHandle handle,
    std::shared_ptr<AsyncQueue<int>> queue,
    int consumerId,
    int itemCount,
    std::shared_ptr<TestResult> result) {
    for (int i = 0; i < itemCount; ++i) {
        int value = co_await queue->waitDequeue();
        result->recordConsumedItem(value);
    }

    co_return nil();
}

// ==================== 单线程测试用例 ====================

// Test 1: 单生产者单消费者 (单线程)
Coroutine<nil> test_single_producer_consumer_st(CoSchedulerHandle handle) {
    auto queue = std::make_shared<AsyncQueue<int>>();
    auto result = std::make_shared<TestResult>();
    result->testName = "ST-SingleProducerConsumer";
    result->producedItems = ST_TEST1_ITEMS;

    handle.spawn(single_thread_producer(handle, queue, 1, ST_TEST1_ITEMS));
    handle.spawn(single_thread_consumer(handle, queue, 1, ST_TEST1_ITEMS, result));

    recordTestResult(result);
    co_return nil();
}

// Test 2: 多生产者单消费者 (单线程)
Coroutine<nil> test_multiple_producers_single_consumer_st(CoSchedulerHandle handle) {
    auto queue = std::make_shared<AsyncQueue<int>>();
    auto result = std::make_shared<TestResult>();
    result->testName = "ST-MultiProducerSingleConsumer";
    result->producedItems = ST_TEST2_PRODUCER_COUNT * ST_TEST2_ITEMS_PER_PRODUCER;

    // Multiple producers × items per producer
    for (int i = 0; i < ST_TEST2_PRODUCER_COUNT; ++i) {
        handle.spawn(single_thread_producer(handle, queue, i + 1, ST_TEST2_ITEMS_PER_PRODUCER));
    }

    // 1 consumer consumes all items
    handle.spawn(single_thread_consumer(handle, queue, 1, ST_TEST2_PRODUCER_COUNT * ST_TEST2_ITEMS_PER_PRODUCER, result));

    recordTestResult(result);
    co_return nil();
}

// ==================== 多线程测试用例（关键）====================

/**
 * @brief Test 3: 多生产者单消费者 (多线程)
 *
 * 这是最关键的测试！用来验证 AsyncQueue 在多线程环境下的安全性
 *
 * 测试场景：
 * - 在多个线程中运行生产者协程
 * - 在一个线程中运行消费者协程
 * - 验证所有数据都被正确消费
 */
Coroutine<nil> test_multiple_producers_single_consumer_mt(
    std::vector<CoSchedulerHandle>& handles) {
    auto queue = std::make_shared<AsyncQueue<int>>();
    auto result = std::make_shared<TestResult>();
    result->testName = "MT-MultiProducerSingleConsumer";
    result->producedItems = MT_TEST3_PRODUCER_COUNT * MT_TEST3_ITEMS_PER_PRODUCER;

    if (handles.size() < 2) {
        result->errorMsg = "Not enough scheduler handles for multi-thread test";
        recordTestResult(result);
        co_return nil();
    }

    // 在不同的线程中运行生产者
    int handle_idx = 0;
    for (int i = 0; i < MT_TEST3_PRODUCER_COUNT; ++i) {
        handles[handle_idx % handles.size()].spawn(
            multi_thread_producer(handles[handle_idx % handles.size()], queue, i + 1, MT_TEST3_ITEMS_PER_PRODUCER)
        );
        handle_idx++;
    }

    // 消费者在另一个线程
    handles[handle_idx % handles.size()].spawn(
        multi_thread_consumer(handles[handle_idx % handles.size()], queue, 1, MT_TEST3_PRODUCER_COUNT * MT_TEST3_ITEMS_PER_PRODUCER, result)
    );

    recordTestResult(result);
    co_return nil();
}

// ==================== 主函数 ====================

int main() {
    // 创建 Runtime（支持多线程）
    RuntimeBuilder builder;
    Runtime runtime = builder
        .startCoManager(std::chrono::milliseconds(1000))
        .setCoSchedulerNum(MT_TEST3_PRODUCER_COUNT)
        .build();
    runtime.start();

    // 获取所有可用的调度器句柄（对应多个线程）
    std::vector<CoSchedulerHandle> all_handles;
    for (int i = 0; i < MT_TEST3_PRODUCER_COUNT; ++i) {
        auto handle = runtime.getCoSchedulerHandle(i);
        if (handle) {
            all_handles.push_back(*handle);
        }
    }

    if (all_handles.empty()) {
        std::cerr << "❌ Failed to get any scheduler handle" << std::endl;
        return 1;
    }

    // ===== 运行所有测试 =====

    // Test 1
    std::cout << "Starting Test 1..." << std::endl;
    g_test_results.clear();
    runtime.schedule(test_single_producer_consumer_st(all_handles[0]));
    std::cout << "Waiting for Test 1 to complete... (press ENTER)" << std::endl;
    getchar();
    std::this_thread::sleep_for(std::chrono::milliseconds(3000));  // Wait for coroutine completion
    std::cout << "\n>>> Test 1 Result (collected " << g_test_results.size() << " results):\n";
    for (const auto& result : g_test_results) {
        result->print();
        g_all_test_results.push_back(result);
    }

    // Test 2
    std::cout << "\nStarting Test 2..." << std::endl;
    g_test_results.clear();
    runtime.schedule(test_multiple_producers_single_consumer_st(all_handles[0]));
    std::cout << "Waiting for Test 2 to complete... (press ENTER)" << std::endl;
    getchar();
    std::this_thread::sleep_for(std::chrono::milliseconds(5000));  // Wait for coroutine completion
    std::cout << "\n>>> Test 2 Result (collected " << g_test_results.size() << " results):\n";
    for (const auto& result : g_test_results) {
        result->print();
        g_all_test_results.push_back(result);
    }

    // Test 3
    std::cout << "\nStarting Test 3..." << std::endl;
    g_test_results.clear();
    runtime.schedule(test_multiple_producers_single_consumer_mt(all_handles));
    std::cout << "Waiting for Test 3 to complete... (press ENTER)" << std::endl;
    getchar();
    std::this_thread::sleep_for(std::chrono::milliseconds(10000));  // Wait for coroutine completion
    std::cout << "\n>>> Test 3 Result (collected " << g_test_results.size() << " results):\n";
    for (const auto& result : g_test_results) {
        result->print();
        g_all_test_results.push_back(result);
    }

    // ===== 结果汇总 =====
    std::cout << "\n╔═══════════════════════════════════════════════════════════════╗" << std::endl;
    std::cout << "║                   FINAL TEST SUMMARY                         ║" << std::endl;
    std::cout << "╚═══════════════════════════════════════════════════════════════╝" << std::endl;

    int pass_count = 0;
    for (const auto& result : g_all_test_results) {
        if (result->verify()) {
            pass_count++;
        }
    }

    std::cout << "\n" << std::string(67, '=') << std::endl;
    std::cout << "Total Tests: " << g_all_test_results.size() << " | "
              << "Passed: " << pass_count << " | "
              << "Failed: " << (g_all_test_results.size() - pass_count) << std::endl;
    std::cout << std::string(67, '=') << std::endl;

    if (pass_count == static_cast<int>(g_all_test_results.size()) && !g_all_test_results.empty()) {
        std::cout << "\n✅ ALL TESTS PASSED - AsyncQueue is working correctly!" << std::endl;
        std::cout << "   ✓ Single thread tests: PASS" << std::endl;
        std::cout << "   ✓ Multi-thread tests: PASS" << std::endl;
        std::cout << "   ✓ No data corruption or loss detected" << std::endl;
    } else {
        std::cout << "\n❌ SOME TESTS FAILED" << std::endl;
        std::cout << "   Please review the results above!" << std::endl;
    }

    runtime.stop();
    return pass_count == static_cast<int>(g_all_test_results.size()) ? 0 : 1;
}
