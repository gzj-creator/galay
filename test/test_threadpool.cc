#include "galay/utils/Thread.h"
#include <iostream>
#include <vector>
#include <cassert>

using namespace galay::thread;

void test_basic_functionality() {
    std::cout << "Testing basic functionality..." << std::endl;
    
    ScrambleThreadPool::uptr pool = std::make_unique<ScrambleThreadPool>();
    pool->start(4);
    
    // 添加简单计算任务
    auto future1 = pool->addTask([]() { 
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        return 42; 
    });
    
    auto future2 = pool->addTask([]() { 
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        return 24; 
    });
    
    // 验证返回值
    assert(future1.get() == 42);
    assert(future2.get() == 24);
    
    pool->stop();
    std::cout << "Basic functionality test passed." << std::endl;
}

void test_concurrent_execution() {
    std::cout << "Testing concurrent execution..." << std::endl;
    
    ScrambleThreadPool::uptr pool = std::make_unique<ScrambleThreadPool>();
    pool->start(4);
    
    const int task_count = 8;
    std::vector<std::future<int>> futures;
    
    auto start_time = std::chrono::high_resolution_clock::now();
    
    // 添加多个耗时任务
    for (int i = 0; i < task_count; ++i) {
        auto future = pool->addTask([i]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            return i;
        });
        futures.push_back(std::move(future));
    }
    
    // 获取所有结果
    for (int i = 0; i < task_count; ++i) {
        assert(futures[i].get() == i);
    }
    
    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
    
    // 如果是串行执行，总时间应该接近 8 * 100ms = 800ms
    // 如果是并发执行，总时间应该明显小于 800ms (取决于线程数)
    // 这里我们期望它在 400ms 到 600ms 之间完成 (基于4个线程)
    assert(duration.count() < 600);
    
    pool->stop();
    std::cout << "Concurrent execution test passed in " << duration.count() << " ms." << std::endl;
}

void test_exception_handling() {
    std::cout << "Testing exception handling..." << std::endl;
    
    ScrambleThreadPool::uptr pool = std::make_unique<ScrambleThreadPool>();
    pool->start(2);
    
    // 添加抛出异常的任务
    auto future = pool->addTask([]() -> int {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        throw std::runtime_error("Test exception");
        return 42;
    });
    
    try {
        future.get(); // 应该抛出异常
        assert(false); // 不应到达这里
    } catch (const std::runtime_error& e) {
        assert(std::string(e.what()) == "Test exception");
    }
    
    // 添加正常任务确保线程池仍然工作正常
    auto normal_future = pool->addTask([]() { 
        return 123; 
    });
    assert(normal_future.get() == 123);
    
    pool->stop();
    std::cout << "Exception handling test passed." << std::endl;
}

void test_multiple_starts_and_stops() {
    std::cout << "Testing multiple starts and stops..." << std::endl;
    
    ScrambleThreadPool::uptr pool = std::make_unique<ScrambleThreadPool>();
    
    for (int i = 0; i < 3; ++i) {
        pool->start(2);
        
        auto future = pool->addTask([i]() {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            return 100 + i;
        });
        
        assert(future.get() == 100 + i);
        pool->stop();
    }
    
    std::cout << "Multiple starts and stops test passed." << std::endl;
}

int main() {
    try {
        test_basic_functionality();
        test_concurrent_execution();
        test_exception_handling();
        test_multiple_starts_and_stops();
        std::cout << "All tests passed!" << std::endl;
    } catch (const std::exception& e) {
        std::cerr << "Test failed with exception: " << e.what() << std::endl;
        return 1;
    }
    
    return 0;
}