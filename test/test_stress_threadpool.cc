/**
 * @file test_stress_threadpool.cc
 * @brief 线程池压力测试
 * @details 测试线程池在高并发任务下的性能表现
 */

#include "galay/utils/Thread.h"
#include <iostream>
#include <atomic>
#include <chrono>
#include <vector>
#include <random>

using namespace galay::thread;
using namespace std::chrono;

// 压测配置
constexpr int THREAD_COUNT = 8;              // 线程池线程数
constexpr int TASK_COUNT = 100000;           // 总任务数
constexpr int COMPUTE_ITERATIONS = 1000;     // 每个任务的计算迭代次数

// 统计数据
std::atomic<uint64_t> tasks_completed{0};
std::atomic<uint64_t> total_compute_result{0};

/**
 * @brief CPU密集型任务
 */
uint64_t cpuIntensiveTask(int task_id) {
    uint64_t result = 0;
    for (int i = 0; i < COMPUTE_ITERATIONS; ++i) {
        result += task_id * i;
        result ^= (result << 13);
        result ^= (result >> 7);
        result ^= (result << 17);
    }
    return result;
}

/**
 * @brief 测试CPU密集型任务
 */
void testCpuIntensiveTasks() {
    std::cout << "\n=== CPU密集型任务压力测试 ===" << std::endl;
    std::cout << "线程数: " << THREAD_COUNT << std::endl;
    std::cout << "任务数: " << TASK_COUNT << std::endl;
    std::cout << "计算迭代: " << COMPUTE_ITERATIONS << std::endl;
    
    ScrambleThreadPool pool;
    pool.start(THREAD_COUNT);
    
    tasks_completed = 0;
    total_compute_result = 0;
    
    auto start_time = high_resolution_clock::now();
    
    // 提交任务
    std::vector<std::future<uint64_t>> futures;
    futures.reserve(TASK_COUNT);
    
    for (int i = 0; i < TASK_COUNT; ++i) {
        futures.push_back(pool.addTask([](int id) {
            uint64_t result = cpuIntensiveTask(id);
            tasks_completed++;
            return result;
        }, i));
    }
    
    // 等待所有任务完成并收集结果
    for (auto& future : futures) {
        total_compute_result += future.get();
    }
    
    auto end_time = high_resolution_clock::now();
    duration<double> elapsed = end_time - start_time;
    
    pool.stop();
    
    std::cout << "完成任务数: " << tasks_completed.load() << std::endl;
    std::cout << "总计算结果: " << total_compute_result.load() << std::endl;
    std::cout << "耗时: " << elapsed.count() << " 秒" << std::endl;
    std::cout << "任务吞吐量: " << (TASK_COUNT / elapsed.count()) << " 任务/秒" << std::endl;
    std::cout << "✓ CPU密集型测试完成\n" << std::endl;
}

/**
 * @brief IO密集型任务模拟（使用短暂睡眠）
 */
void ioIntensiveTask(int task_id) {
    // 模拟IO等待
    std::this_thread::sleep_for(std::chrono::microseconds(100));
    tasks_completed++;
}

/**
 * @brief 测试IO密集型任务
 */
void testIoIntensiveTasks() {
    std::cout << "\n=== IO密集型任务压力测试 ===" << std::endl;
    std::cout << "线程数: " << THREAD_COUNT * 2 << std::endl;
    std::cout << "任务数: " << TASK_COUNT / 10 << " (较少任务，因为有IO等待)" << std::endl;
    
    ScrambleThreadPool pool;
    pool.start(THREAD_COUNT * 2);  // IO密集型通常需要更多线程
    
    tasks_completed = 0;
    
    auto start_time = high_resolution_clock::now();
    
    // 提交任务
    std::vector<std::future<void>> futures;
    int io_task_count = TASK_COUNT / 10;
    futures.reserve(io_task_count);
    
    for (int i = 0; i < io_task_count; ++i) {
        futures.push_back(pool.addTask(ioIntensiveTask, i));
    }
    
    // 等待所有任务完成
    for (auto& future : futures) {
        future.wait();
    }
    
    auto end_time = high_resolution_clock::now();
    duration<double> elapsed = end_time - start_time;
    
    pool.stop();
    
    std::cout << "完成任务数: " << tasks_completed.load() << std::endl;
    std::cout << "耗时: " << elapsed.count() << " 秒" << std::endl;
    std::cout << "任务吞吐量: " << (io_task_count / elapsed.count()) << " 任务/秒" << std::endl;
    std::cout << "✓ IO密集型测试完成\n" << std::endl;
}

/**
 * @brief 测试混合工作负载
 */
void testMixedWorkload() {
    std::cout << "\n=== 混合工作负载压力测试 ===" << std::endl;
    std::cout << "线程数: " << THREAD_COUNT << std::endl;
    std::cout << "任务数: " << TASK_COUNT / 5 << std::endl;
    
    ScrambleThreadPool pool;
    pool.start(THREAD_COUNT);
    
    tasks_completed = 0;
    
    auto start_time = high_resolution_clock::now();
    
    // 提交混合任务
    std::vector<std::future<void>> futures;
    int mixed_task_count = TASK_COUNT / 5;
    futures.reserve(mixed_task_count);
    
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 1);
    
    for (int i = 0; i < mixed_task_count; ++i) {
        if (dis(gen) == 0) {
            // CPU密集型
            futures.push_back(pool.addTask([](int id) {
                cpuIntensiveTask(id);
                tasks_completed++;
            }, i));
        } else {
            // IO密集型
            futures.push_back(pool.addTask(ioIntensiveTask, i));
        }
    }
    
    // 等待所有任务完成
    for (auto& future : futures) {
        future.wait();
    }
    
    auto end_time = high_resolution_clock::now();
    duration<double> elapsed = end_time - start_time;
    
    pool.stop();
    
    std::cout << "完成任务数: " << tasks_completed.load() << std::endl;
    std::cout << "耗时: " << elapsed.count() << " 秒" << std::endl;
    std::cout << "任务吞吐量: " << (mixed_task_count / elapsed.count()) << " 任务/秒" << std::endl;
    std::cout << "✓ 混合工作负载测试完成\n" << std::endl;
}

/**
 * @brief 测试线程池扩展性
 */
void testScalability() {
    std::cout << "\n=== 线程池扩展性测试 ===" << std::endl;
    
    const int test_task_count = 10000;
    std::vector<int> thread_counts = {1, 2, 4, 8, 16};
    
    for (int threads : thread_counts) {
        ScrambleThreadPool pool;
        pool.start(threads);
        
        auto start_time = high_resolution_clock::now();
        
        std::vector<std::future<uint64_t>> futures;
        futures.reserve(test_task_count);
        
        for (int i = 0; i < test_task_count; ++i) {
            futures.push_back(pool.addTask(cpuIntensiveTask, i));
        }
        
        for (auto& future : futures) {
            future.wait();
        }
        
        auto end_time = high_resolution_clock::now();
        duration<double> elapsed = end_time - start_time;
        
        pool.stop();
        
        std::cout << threads << " 线程: " << elapsed.count() << " 秒, "
                  << (test_task_count / elapsed.count()) << " 任务/秒" << std::endl;
    }
    
    std::cout << "✓ 扩展性测试完成\n" << std::endl;
}

int main() {
    std::cout << "线程池压力测试" << std::endl;
    std::cout << "================================" << std::endl;
    std::cout << "硬件并发数: " << std::thread::hardware_concurrency() << std::endl;
    
    try {
        testCpuIntensiveTasks();
        testIoIntensiveTasks();
        testMixedWorkload();
        testScalability();
        
        std::cout << "================================" << std::endl;
        std::cout << "✓ 所有压力测试完成！" << std::endl;
        
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "✗ 测试失败: " << e.what() << std::endl;
        return 1;
    }
}

