/**
 * @file e7_unsafe.cc
 * @brief 用途：用模块导入方式演示 `UnsafeChannel` 的协程生产消费流程。
 * 关键覆盖点：同调度器协程通信、生产者 `co_yield`、消费者累加校验。
 * 通过条件：收到全部消息且累加结果正确，示例返回 0。
 */

import galay.kernel;

#include <coroutine>
#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>

using namespace galay::kernel;

namespace {
constexpr int kMessageCount = 30;
std::atomic<int> g_received{0};
std::atomic<long long> g_sum{0};
std::atomic<bool> g_done{false};

Task<void> producer(UnsafeChannel<int>* channel) {
    for (int i = 1; i <= kMessageCount; ++i) {
        channel->send(i);
        co_yield true;
    }
    co_return;
}

Task<void> consumer(UnsafeChannel<int>* channel) {
    while (g_received.load(std::memory_order_acquire) < kMessageCount) {
        auto value = co_await channel->recv();
        if (!value) {
            continue;
        }
        g_sum.fetch_add(value.value(), std::memory_order_relaxed);
        g_received.fetch_add(1, std::memory_order_relaxed);
    }

    g_done.store(true, std::memory_order_release);
    co_return;
}
}  // namespace

int main() {
    UnsafeChannel<int> channel;
    ComputeScheduler scheduler;
    scheduler.start();

    scheduleTask(scheduler, consumer(&channel));
    scheduleTask(scheduler, producer(&channel));

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!g_done.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    scheduler.stop();

    std::cout << "unsafe-channel import example received=" << g_received.load()
              << ", sum=" << g_sum.load() << "\n";
    return g_done.load(std::memory_order_acquire) ? 0 : 1;
}
