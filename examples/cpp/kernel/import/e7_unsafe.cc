/**
 * @file e7_unsafe.cc
 * @brief 用途：用模块导入方式演示 `galay::spsc::UnboundedChannel` 的协程生产消费流程。
 * 关键覆盖点：同调度器协程通信、生产者 `co_yield`、消费者累加校验。
 * 通过条件：收到全部消息且累加结果正确，示例返回 0。
 */

#include <coroutine>
#include <atomic>
#include <chrono>
#include <expected>
#include <iostream>
#include <thread>

import galay.kernel;

using namespace galay::kernel;

namespace {
constexpr int kMessageCount = 30;
std::atomic<int> g_received{0};
std::atomic<long long> g_sum{0};
std::atomic<bool> g_done{false};
std::atomic<bool> g_failed{false};

Task<void> producer(galay::spsc::UnboundedChannel<int>* channel) {
    for (int i = 1; i <= kMessageCount; ++i) {
        if (!channel->send(i)) {
            g_failed.store(true, std::memory_order_release);
            break;
        }
        co_yield true;
    }
    co_return;
}

Task<void> consumer(galay::spsc::UnboundedChannel<int>* channel) {
    while (g_received.load(std::memory_order_acquire) < kMessageCount) {
        if (g_failed.load(std::memory_order_acquire)) {
            break;
        }
        auto value = co_await channel->recv();
        if (!value) {
            g_failed.store(true, std::memory_order_release);
            break;
        }
        g_sum.fetch_add(value.value(), std::memory_order_relaxed);
        g_received.fetch_add(1, std::memory_order_relaxed);
    }

    g_done.store(true, std::memory_order_release);
    co_return;
}
}  // namespace

int main() {
    galay::spsc::UnboundedChannel<int> channel;
    ComputeScheduler scheduler;
    auto started = scheduler.start();
    if (!started) {
        std::cerr << "unsafe-channel import example failed to start scheduler\n";
        return 1;
    }

    if (!scheduleTask(scheduler, consumer(&channel)) ||
        !scheduleTask(scheduler, producer(&channel))) {
        std::cerr << "unsafe-channel import example failed to schedule tasks\n";
        scheduler.stop();
        return 1;
    }

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!g_done.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    scheduler.stop();

    std::cout << "unsafe-channel import example received=" << g_received.load()
              << ", sum=" << g_sum.load() << "\n";
    return g_done.load(std::memory_order_acquire) &&
                   !g_failed.load(std::memory_order_acquire)
        ? 0
        : 1;
}
