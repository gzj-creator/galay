/**
 * @file t33_batchrecv.cc
 * @brief 用途：验证 `MpscChannel` 在批量接收压力下的正确性与稳定性。
 * 关键覆盖点：高并发发送、批量消费、累计计数与唤醒稳定性。
 * 通过条件：压力下无丢消息或卡死，测试返回 0。
 */

#include "kernel/concurrency/mpsc_channel.h"
#include "kernel/kernel/compute_scheduler.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <thread>

using namespace galay::kernel;
using namespace std::chrono_literals;

namespace {

constexpr int64_t kMessageCount = 1'000'000;

std::atomic<bool> g_consumer_done{false};
std::atomic<bool> g_producer_done{false};
std::atomic<int64_t> g_received{0};

bool waitUntil(const std::atomic<bool>& flag,
               std::chrono::milliseconds timeout = 5000ms,
               std::chrono::milliseconds step = 2ms) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (flag.load(std::memory_order_acquire)) {
            return true;
        }
        std::this_thread::sleep_for(step);
    }
    return flag.load(std::memory_order_acquire);
}

Task<void> batchConsumer(MpscChannel<int64_t>* channel) {
    int64_t received = 0;
    while (received < kMessageCount) {
        auto batch = co_await channel->recvBatch(256);
        if (!batch) {
            continue;
        }
        received += static_cast<int64_t>(batch->size());
        g_received.store(received, std::memory_order_release);
    }

    g_consumer_done.store(true, std::memory_order_release);
    co_return;
}

}  // namespace

int main() {
    MpscChannel<int64_t> channel;
    ComputeScheduler scheduler;
    scheduler.start();
    scheduler.schedule(detail::TaskAccess::detachTask(batchConsumer(&channel)));

    std::thread producer([&]() {
        for (int64_t i = 0; i < kMessageCount; ++i) {
            channel.send(i);
        }
        g_producer_done.store(true, std::memory_order_release);
    });

    const bool done = waitUntil(g_consumer_done);
    producer.join();
    scheduler.stop();

    if (!done) {
        std::cerr << "[T33] batch consumer stalled, producer_done="
                  << g_producer_done.load(std::memory_order_acquire)
                  << " received=" << g_received.load(std::memory_order_acquire)
                  << " channel.size=" << channel.size() << "\n";
        return 1;
    }

    if (g_received.load(std::memory_order_acquire) != kMessageCount) {
        std::cerr << "[T33] expected received=" << kMessageCount
                  << ", got " << g_received.load(std::memory_order_acquire) << "\n";
        return 1;
    }

    std::cout << "T33-MpscBatchReceiveStress PASS\n";
    return 0;
}
