/**
 * @file t33_batchrecv.cc
 * @brief 用途：验证 `galay::mpsc::UnboundedChannel` 在批量接收压力下的正确性与稳定性。
 * 关键覆盖点：高并发发送、批量消费、累计计数与唤醒稳定性。
 * 通过条件：压力下无丢消息或卡死，测试返回 0。
 */

#include <galay/cpp/galay-kernel/concurrency/mpsc/unbounded_channel.h>
#include <galay/cpp/galay-kernel/core/compute_scheduler.h>

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
std::atomic<bool> g_send_failed{false};
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

Task<void> batchConsumer(galay::mpsc::UnboundedChannel<int64_t>* channel) {
    int64_t received = 0;
    while (received < kMessageCount) {
        auto batch = co_await channel->recvBatch(256);
        if (!batch) {
            if (g_send_failed.load(std::memory_order_acquire) ||
                channel->isClosed()) {
                g_consumer_done.store(true, std::memory_order_release);
                co_return;
            }
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
    galay::mpsc::UnboundedChannel<int64_t> channel;
    ComputeScheduler scheduler;
    const auto schedulerStarted = scheduler.start();
    if (!schedulerStarted.has_value()) {
        std::cerr << "[T33] failed to start consumer scheduler\n";
        return 1;
    }
    const bool scheduled = scheduler.schedule(
        detail::TaskAccess::detachTask(batchConsumer(&channel)));
    if (!scheduled) {
        scheduler.stop();
        std::cerr << "[T33] failed to schedule batch consumer\n";
        return 1;
    }

    std::thread producer([&]() {
        for (int64_t i = 0; i < kMessageCount; ++i) {
            if (!channel.send(i)) {
                g_send_failed.store(true, std::memory_order_release);
                if (!channel.close() && !channel.isClosed()) {
                    g_send_failed.store(true, std::memory_order_release);
                }
                break;
            }
        }
        g_producer_done.store(true, std::memory_order_release);
    });

    const bool done = waitUntil(g_consumer_done);
    producer.join();
    scheduler.stop();

    if (!done) {
        std::cerr << "[T33] batch consumer stalled, send_failed="
                  << g_send_failed.load(std::memory_order_acquire)
                  << " producer_done=" << g_producer_done.load(std::memory_order_acquire)
                  << " received=" << g_received.load(std::memory_order_acquire)
                  << " channel.size=" << channel.size() << "\n";
        return 1;
    }

    if (g_send_failed.load(std::memory_order_acquire) ||
        g_received.load(std::memory_order_acquire) != kMessageCount) {
        std::cerr << "[T33] send_failed="
                  << g_send_failed.load(std::memory_order_acquire)
                  << ", expected received=" << kMessageCount
                  << ", got " << g_received.load(std::memory_order_acquire) << "\n";
        return 1;
    }

    std::cout << "T33-MpscBatchReceiveStress PASS\n";
    return 0;
}
