/**
 * @file e6_mpsc.cc
 * @brief 用途：用模块导入方式演示 `galay::mpsc::UnboundedChannel` 的多生产者单消费者用法。
 * 关键覆盖点：跨线程发送、协程接收、累计求和与消息总数统计。
 * 通过条件：收到预期消息总数且统计正确，示例返回 0。
 */

#include <coroutine>
#include <atomic>
#include <chrono>
#include <expected>
#include <iostream>
#include <thread>
#include <vector>

import galay.kernel;

using namespace galay::kernel;

namespace {
constexpr int kProducerCount = 3;
constexpr int kMessagesPerProducer = 20;
constexpr int kExpectedTotal = kProducerCount * kMessagesPerProducer;

std::atomic<int> g_received{0};
std::atomic<long long> g_sum{0};
std::atomic<bool> g_done{false};
std::atomic<bool> g_failed{false};

Task<void> consumer(galay::mpsc::UnboundedChannel<int>* channel) {
    while (g_received.load(std::memory_order_acquire) < kExpectedTotal) {
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

void producer(galay::mpsc::UnboundedChannel<int>* channel, int producerId) {
    for (int i = 1; i <= kMessagesPerProducer; ++i) {
        const int value = producerId * 100 + i;
        if (!channel->send(value)) {
            g_failed.store(true, std::memory_order_release);
            if (!channel->close() && !channel->isClosed()) {
                std::cerr << "mpsc import example failed to close channel\n";
            }
            return;
        }
    }
}
}  // namespace

int main() {
    galay::mpsc::UnboundedChannel<int> channel;
    ParallelScheduler scheduler;
    auto started = scheduler.start();
    if (!started) {
        std::cerr << "mpsc import example failed to start scheduler\n";
        return 1;
    }

    if (!scheduleTask(scheduler, consumer(&channel))) {
        std::cerr << "mpsc import example failed to schedule consumer\n";
        scheduler.stop();
        return 1;
    }

    std::vector<std::thread> producers;
    producers.reserve(kProducerCount);
    for (int i = 0; i < kProducerCount; ++i) {
        producers.emplace_back(producer, &channel, i + 1);
    }
    for (auto& thread : producers) {
        thread.join();
    }

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!g_done.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    scheduler.stop();

    std::cout << "mpsc import example received=" << g_received.load()
              << ", sum=" << g_sum.load() << "\n";
    return g_done.load(std::memory_order_acquire) &&
                   !g_failed.load(std::memory_order_acquire)
        ? 0
        : 1;
}
