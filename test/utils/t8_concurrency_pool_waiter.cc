#include "test_common.hpp"

#include <filesystem>
#include <fstream>
#include <sstream>

void testConcurrencyHeadersMovedToTool() {
    const auto sourceRoot = std::filesystem::path(GALAY_UTILS_SOURCE_DIR);
    assert(!std::filesystem::exists(sourceRoot / "galay-utils/concurrency/pool.hpp"));
    assert(!std::filesystem::exists(sourceRoot / "galay-utils/concurrency/thread.hpp"));
    assert(std::filesystem::exists(sourceRoot / "galay-utils/tool/pool.hpp"));
    assert(std::filesystem::exists(sourceRoot / "galay-utils/tool/thread.hpp"));
}

void testPool() {
    std::cout << "=== Testing Pool ===" << std::endl;

    // Basic object pool
    ObjectPool<std::string> pool(5, 10);
    assert(pool.size() == 5);

    {
        auto obj1 = pool.acquire();
        *obj1 = "test";
        assert(pool.size() == 4);

        auto obj2 = pool.acquire();
        assert(pool.size() == 3);
    }
    // Objects returned to pool
    assert(pool.size() == 5);

    // Blocking pool
    BlockingObjectPool<int> blockingPool(3);
    assert(blockingPool.available() == 3);

    {
        auto obj = blockingPool.acquire();
        assert(blockingPool.available() == 2);
    }
    assert(blockingPool.available() == 3);

    std::cout << "Pool tests passed!" << std::endl;
}

// ==================== Thread Tests ====================

void testThreadPoolUsesConcurrentQueueWithoutMutex() {
    std::ifstream input(std::string(GALAY_UTILS_SOURCE_DIR) + "/galay-utils/tool/thread.hpp");
    assert(input.good());

    std::ostringstream buffer;
    buffer << input.rdbuf();
    const std::string source = buffer.str();

    const auto threadPoolStart = source.find("class ThreadPool");
    const auto waiterStart = source.find("class TaskWaiter");
    const auto removedListNode = source.find("struct ListNode");
    const auto removedThreadSafeList = source.find("class ThreadSafeList");
    assert(threadPoolStart != std::string::npos);
    assert(waiterStart != std::string::npos);
    assert(removedListNode == std::string::npos);
    assert(removedThreadSafeList == std::string::npos);

    const std::string threadPoolSource = source.substr(threadPoolStart, waiterStart - threadPoolStart);
    const std::string waiterSource = source.substr(waiterStart);

    assert(threadPoolSource.find("moodycamel::BlockingConcurrentQueue") != std::string::npos);
    assert(threadPoolSource.find("std::mutex") == std::string::npos);
    assert(threadPoolSource.find("std::condition_variable") == std::string::npos);
    assert(threadPoolSource.find("lock_guard") == std::string::npos);
    assert(threadPoolSource.find("unique_lock") == std::string::npos);

    assert(waiterSource.find("std::mutex") == std::string::npos);
    assert(waiterSource.find("std::condition_variable") == std::string::npos);
    assert(waiterSource.find("lock_guard") == std::string::npos);
    assert(waiterSource.find("unique_lock") == std::string::npos);
}

void testThread() {
    std::cout << "=== Testing Thread ===" << std::endl;

    ThreadPool pool(4);
    assert(pool.threadCount() == 4);

    // Add tasks with futures
    std::vector<std::future<int>> futures;
    for (int i = 0; i < 10; ++i) {
        futures.push_back(pool.addTask([i]() {
            return i * i;
        }));
    }

    // Check results
    for (int i = 0; i < 10; ++i) {
        assert(futures[i].get() == i * i);
    }

    // Task waiter
    TaskWaiter waiter;
    std::atomic<int> counter{0};

    for (int i = 0; i < 5; ++i) {
        waiter.addTask(pool, [&counter]() {
            ++counter;
        });
    }

    waiter.wait();
    assert(counter == 5);

    // Edge cases for thread pool
    // Zero thread pool
    ThreadPool zeroPool(0);
    assert(zeroPool.threadCount() > 0); // Should default to hardware concurrency

    // Empty task list
    TaskWaiter emptyWaiter;
    emptyWaiter.wait(); // Should not hang

    ThreadPool stoppedPool(1);
    stoppedPool.stop();

    bool addTaskRejected = false;
    try {
        (void)stoppedPool.addTask([] { return 1; });
    } catch (const std::runtime_error&) {
        addTaskRejected = true;
    }
    assert(addTaskRejected);

    std::atomic<int> ignored{0};
    stoppedPool.execute([&ignored] {
        ++ignored;
    });
    assert(ignored == 0);

    std::cout << "Thread tests passed!" << std::endl;
}

// ==================== RateLimiter Tests ====================

void stressTestPool() {
    std::cout << "=== Stress Testing Pool ===" << std::endl;

    BlockingObjectPool<int> pool(100);
    const int numThreads = 8;
    const int opsPerThread = 50000;
    std::atomic<int> acquired{0};

    auto start = std::chrono::high_resolution_clock::now();

    std::vector<std::thread> threads;
    for (int t = 0; t < numThreads; ++t) {
        threads.emplace_back([&]() {
            for (int i = 0; i < opsPerThread; ++i) {
                auto obj = pool.tryAcquireFor(std::chrono::microseconds(1));
                if (obj) {
                    ++acquired;
                    *obj = i;
                }
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    int totalOps = numThreads * opsPerThread;
    double opsPerSec = (totalOps * 1000.0) / duration;

    std::cout << "  Threads: " << numThreads << std::endl;
    std::cout << "  Total ops: " << totalOps << std::endl;
    std::cout << "  Duration: " << duration << "ms" << std::endl;
    std::cout << "  Throughput: " << std::fixed << std::setprecision(0) << opsPerSec << " ops/sec" << std::endl;
    std::cout << "  Acquired: " << acquired << std::endl;
    std::cout << "  Pool available: " << pool.available() << std::endl;

    assert(pool.available() == 100);

    std::cout << "Pool stress test passed!" << std::endl;
}

void stressTestThreadPool() {
    std::cout << "=== Stress Testing ThreadPool ===" << std::endl;

    ThreadPool pool(8);
    const int numTasks = 100000;
    std::atomic<int> completed{0};

    auto start = std::chrono::high_resolution_clock::now();

    for (int i = 0; i < numTasks; ++i) {
        pool.execute([&completed]() {
            ++completed;
        });
    }

    pool.waitAll();

    auto end = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    double tasksPerSec = (numTasks * 1000.0) / duration;

    std::cout << "  Thread count: " << pool.threadCount() << std::endl;
    std::cout << "  Total tasks: " << numTasks << std::endl;
    std::cout << "  Duration: " << duration << "ms" << std::endl;
    std::cout << "  Throughput: " << std::fixed << std::setprecision(0) << tasksPerSec << " tasks/sec" << std::endl;
    std::cout << "  Completed: " << completed << std::endl;

    assert(completed == numTasks);

    std::cout << "ThreadPool stress test passed!" << std::endl;
}

// ==================== Main ====================

int main() {
    std::cout << "\n=== concurrency_test ===" << std::endl;
    try {
        testConcurrencyHeadersMovedToTool();
        testPool();
        testThreadPoolUsesConcurrentQueueWithoutMutex();
        testThread();
        stressTestPool();
        stressTestThreadPool();
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Test failed with exception: " << e.what() << std::endl;
        return 1;
    }
}
