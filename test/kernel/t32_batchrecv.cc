/**
 * @file t32_batchrecv.cc
 * @brief 用途：验证 `MpscChannel` 批量接收接口能够持续推进消费进度。
 * 关键覆盖点：批量 drain backlog、单轮进度推进、无消息时等待再恢复。
 * 通过条件：批量接收不会停滞且统计符合预期，测试返回 0。
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

constexpr int64_t kMessageCount = 100000;

std::atomic<bool> g_done{false};
std::atomic<int64_t> g_received{0};
std::atomic<int64_t> g_sum{0};

bool waitUntil(const std::atomic<bool>& flag,
               std::chrono::milliseconds timeout = 3000ms,
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
    int64_t sum = 0;
    while (received < kMessageCount) {
        auto batch = co_await channel->recvBatch(256);
        if (!batch) {
            continue;
        }
        for (int64_t value : *batch) {
            sum += value;
        }
        received += static_cast<int64_t>(batch->size());
        g_received.store(received, std::memory_order_release);
        g_sum.store(sum, std::memory_order_release);
    }

    g_done.store(true, std::memory_order_release);
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
    });

    const bool done = waitUntil(g_done);
    producer.join();
    scheduler.stop();

    if (!done) {
        const auto tail = channel.tryRecvBatch(256);
        std::cerr << "[T32] batch consumer did not make progress to completion, received="
                  << g_received.load(std::memory_order_acquire)
                  << " channel.size=" << channel.size()
                  << " tail_batch=" << (tail ? tail->size() : 0) << "\n";
        return 1;
    }

    const int64_t expected_sum = (kMessageCount - 1) * kMessageCount / 2;
    if (g_received.load(std::memory_order_acquire) != kMessageCount ||
        g_sum.load(std::memory_order_acquire) != expected_sum) {
        std::cerr << "[T32] expected received=" << kMessageCount
                  << " sum=" << expected_sum
                  << ", got received=" << g_received.load(std::memory_order_acquire)
                  << " sum=" << g_sum.load(std::memory_order_acquire) << "\n";
        return 1;
    }

    std::cout << "T32-MpscBatchReceiveProgress PASS\n";
    return 0;
}
