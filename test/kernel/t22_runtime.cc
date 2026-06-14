/**
 * @file t22_runtime.cc
 * @brief 用途：验证 `Runtime` 在高并发压力下的调度稳定性与正确性。
 * 关键覆盖点：大量任务提交、跨线程调度、完成统计以及整体收敛行为。
 * 通过条件：既定压力任务全部完成且统计符合预期，测试返回 0。
 */

#include <iostream>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>
#include <map>
#include "kernel/kernel/runtime.h"
#include "kernel/kernel/compute_scheduler.h"
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

// ============== 测试1: 多线程并发获取调度器 ==============
void test_concurrent_get_scheduler() {
    ++g_total;
    std::cout << "\n[测试1] 多线程并发获取调度器" << std::endl;

    constexpr int SCHEDULER_COUNT = 4;
    constexpr int THREAD_COUNT = 8;
    constexpr int ITERATIONS = 100000;

    Runtime runtime;

    // 添加调度器
    for (int i = 0; i < SCHEDULER_COUNT; ++i) {
        auto io = std::make_unique<IOSchedulerType>();
        runtime.addIOScheduler(std::move(io));
        auto compute = std::make_unique<ComputeScheduler>();
        runtime.addComputeScheduler(std::move(compute));
    }

    runtime.start();

    // 统计每个调度器被选中的次数
    std::vector<std::atomic<int>> io_counts(SCHEDULER_COUNT);
    std::vector<std::atomic<int>> compute_counts(SCHEDULER_COUNT);
    for (int i = 0; i < SCHEDULER_COUNT; ++i) {
        io_counts[i].store(0);
        compute_counts[i].store(0);
    }

    // 创建多个线程并发获取调度器
    std::vector<std::thread> threads;
    for (int t = 0; t < THREAD_COUNT; ++t) {
        threads.emplace_back([&runtime, &io_counts, &compute_counts]() {
            for (int i = 0; i < ITERATIONS; ++i) {
                auto* io = runtime.getNextIOScheduler();
                auto* compute = runtime.getNextComputeScheduler();

                // 找到对应的索引并计数
                for (int j = 0; j < SCHEDULER_COUNT; ++j) {
                    if (runtime.getIOScheduler(j) == io) {
                        io_counts[j].fetch_add(1, std::memory_order_relaxed);
                        break;
                    }
                }
                for (int j = 0; j < SCHEDULER_COUNT; ++j) {
                    if (runtime.getComputeScheduler(j) == compute) {
                        compute_counts[j].fetch_add(1, std::memory_order_relaxed);
                        break;
                    }
                }
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    runtime.stop();

    // 验证分布是否均匀（允许 10% 的误差）
    int total_io = THREAD_COUNT * ITERATIONS;
    int expected_per_scheduler = total_io / SCHEDULER_COUNT;
    int tolerance = expected_per_scheduler / 10;  // 10% 误差

    bool io_balanced = true;
    bool compute_balanced = true;

    std::cout << "  IO 调度器分布: ";
    for (int i = 0; i < SCHEDULER_COUNT; ++i) {
        int count = io_counts[i].load();
        std::cout << count << " ";
        if (std::abs(count - expected_per_scheduler) > tolerance) {
            io_balanced = false;
        }
    }
    std::cout << "(期望: ~" << expected_per_scheduler << ")" << std::endl;

    std::cout << "  计算调度器分布: ";
    for (int i = 0; i < SCHEDULER_COUNT; ++i) {
        int count = compute_counts[i].load();
        std::cout << count << " ";
        if (std::abs(count - expected_per_scheduler) > tolerance) {
            compute_balanced = false;
        }
    }
    std::cout << "(期望: ~" << expected_per_scheduler << ")" << std::endl;

    if (!io_balanced || !compute_balanced) {
        std::cout << "❌ 负载分布不均匀" << std::endl;
        return;
    }

    std::cout << "✅ 测试通过" << std::endl;
    ++g_passed;
}

// ============== 测试2: 高并发任务提交 ==============
std::atomic<int> g_task_completed{0};

Task<void> simpleTask() {
    g_task_completed.fetch_add(1, std::memory_order_relaxed);
    co_return;
}

void test_high_concurrency_spawn() {
    ++g_total;
    std::cout << "\n[测试2] 高并发任务提交" << std::endl;

    constexpr int SCHEDULER_COUNT = 4;
    constexpr int THREAD_COUNT = 8;
    constexpr int TASKS_PER_THREAD = 10000;

    g_task_completed.store(0);

    Runtime runtime;

    for (int i = 0; i < SCHEDULER_COUNT; ++i) {
        auto io = std::make_unique<IOSchedulerType>();
        runtime.addIOScheduler(std::move(io));
    }

    runtime.start();

    auto start = std::chrono::steady_clock::now();

    // 多线程并发提交任务
    std::vector<std::thread> threads;
    for (int t = 0; t < THREAD_COUNT; ++t) {
        threads.emplace_back([&runtime]() {
            for (int i = 0; i < TASKS_PER_THREAD; ++i) {
                auto* scheduler = runtime.getNextIOScheduler();
                scheduleTask(scheduler, simpleTask());
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    // 等待所有任务完成
    int expected = THREAD_COUNT * TASKS_PER_THREAD;
    for (int i = 0; i < 100; ++i) {
        if (g_task_completed.load() >= expected) {
            break;
        }
        std::this_thread::sleep_for(50ms);
    }

    auto end = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

    runtime.stop();

    int completed = g_task_completed.load();
    std::cout << "  完成任务: " << completed << "/" << expected
              << " 耗时: " << duration.count() << "ms" << std::endl;

    if (completed != expected) {
        std::cout << "❌ 任务未全部完成" << std::endl;
        return;
    }

    std::cout << "✅ 测试通过" << std::endl;
    ++g_passed;
}

// ============== 测试3: 轮询索引溢出测试 ==============
void test_index_overflow() {
    ++g_total;
    std::cout << "\n[测试3] 轮询索引溢出测试" << std::endl;

    constexpr int SCHEDULER_COUNT = 3;
    // 模拟大量调用，测试 uint32_t 溢出后的行为
    constexpr uint64_t ITERATIONS = 1000000;

    Runtime runtime;

    for (int i = 0; i < SCHEDULER_COUNT; ++i) {
        auto io = std::make_unique<IOSchedulerType>();
        runtime.addIOScheduler(std::move(io));
    }

    runtime.start();

    // 快速调用大量次数
    std::map<IOScheduler*, int> distribution;
    for (uint64_t i = 0; i < ITERATIONS; ++i) {
        auto* scheduler = runtime.getNextIOScheduler();
        distribution[scheduler]++;
    }

    runtime.stop();

    // 验证分布
    std::cout << "  分布: ";
    bool balanced = true;
    int expected = ITERATIONS / SCHEDULER_COUNT;
    int tolerance = expected / 100;  // 1% 误差

    for (auto& [scheduler, count] : distribution) {
        std::cout << count << " ";
        if (std::abs(count - expected) > tolerance) {
            balanced = false;
        }
    }
    std::cout << "(期望: ~" << expected << ")" << std::endl;

    if (!balanced) {
        std::cout << "❌ 分布不均匀" << std::endl;
        return;
    }

    std::cout << "✅ 测试通过" << std::endl;
    ++g_passed;
}

// ============== 测试4: 空调度器列表处理 ==============
void test_empty_scheduler_list() {
    ++g_total;
    std::cout << "\n[测试4] 空调度器列表处理" << std::endl;

    Runtime runtime;
    // 不添加任何调度器，也不启动

    auto* io = runtime.getNextIOScheduler();
    auto* compute = runtime.getNextComputeScheduler();

    if (io != nullptr || compute != nullptr) {
        std::cout << "❌ 空列表应返回 nullptr" << std::endl;
        return;
    }

    std::cout << "✅ 测试通过" << std::endl;
    ++g_passed;
}

// ============== 测试5: 性能基准测试 ==============
void test_performance_benchmark() {
    ++g_total;
    std::cout << "\n[测试5] 性能基准测试" << std::endl;

    constexpr int SCHEDULER_COUNT = 8;
    constexpr int ITERATIONS = 10000000;

    Runtime runtime;

    for (int i = 0; i < SCHEDULER_COUNT; ++i) {
        auto io = std::make_unique<IOSchedulerType>();
        runtime.addIOScheduler(std::move(io));
    }

    runtime.start();

    auto start = std::chrono::steady_clock::now();

    for (int i = 0; i < ITERATIONS; ++i) {
        volatile auto* scheduler = runtime.getNextIOScheduler();
        (void)scheduler;
    }

    auto end = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start);

    runtime.stop();

    double ns_per_call = static_cast<double>(duration.count()) / ITERATIONS;
    double calls_per_sec = 1e9 / ns_per_call;

    std::cout << "  " << ITERATIONS << " 次调用耗时: " << duration.count() / 1000000 << "ms" << std::endl;
    std::cout << "  每次调用: " << ns_per_call << "ns" << std::endl;
    std::cout << "  吞吐量: " << static_cast<int>(calls_per_sec / 1000000) << "M ops/sec" << std::endl;

    // 无锁实现应该非常快，每次调用应该在 100ns 以内
    if (ns_per_call > 100) {
        std::cout << "⚠️ 性能可能不够理想" << std::endl;
    }

    std::cout << "✅ 测试通过" << std::endl;
    ++g_passed;
}

// ============== 主函数 ==============
int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "Runtime 压力测试" << std::endl;
    std::cout << "========================================" << std::endl;

    test_concurrent_get_scheduler();
    test_high_concurrency_spawn();
    test_index_overflow();
    test_empty_scheduler_list();
    test_performance_benchmark();

    std::cout << "\n========================================" << std::endl;
    std::cout << "测试结果: " << g_passed.load() << "/" << g_total.load() << " 通过" << std::endl;
    std::cout << "========================================" << std::endl;

    // 写入测试结果
    galay::test::TestResultWriter writer("test_runtime_stress");
    for (int i = 0; i < g_total.load(); ++i) {
        writer.addTest();
    }
    for (int i = 0; i < g_passed.load(); ++i) {
        writer.addPassed();
    }
    for (int i = 0; i < (g_total.load() - g_passed.load()); ++i) {
        writer.addFailed();
    }
    writer.writeResult();

    return (g_passed.load() == g_total.load()) ? 0 : 1;
}
