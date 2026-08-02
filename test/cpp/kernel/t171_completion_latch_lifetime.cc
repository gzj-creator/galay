/**
 * @file t171_completion_latch_lifetime.cc
 * @brief 验证 CompletionLatch 报告完成后不再有 arrival 访问其同步对象。
 */

#include <atomic>
#include <cstddef>
#include <iostream>
#include <thread>

#include "benchmark/cpp/common/benchmark_sync.h"

int main()
{
    constexpr std::size_t kIterations = 2000;

    for (std::size_t iteration = 0; iteration < kIterations; ++iteration) {
        std::atomic<bool> start{false};
        std::thread worker;
        {
            galay::benchmark::CompletionLatch latch(1);
            worker = std::thread([&]() {
                while (!start.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }
                latch.arrive();
            });

            start.store(true, std::memory_order_release);
            while (!latch.ready()) {
                std::this_thread::yield();
            }
        }
        worker.join();
    }

    std::cout << "T171-CompletionLatchLifetime PASS\n";
    return 0;
}
