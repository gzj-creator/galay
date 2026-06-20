/**
 * @file t26_uwake.cc
 * @brief 用途：验证 `UnsafeChannel` 延迟唤醒后消费者仍能继续接收数据。
 * 关键覆盖点：延迟唤醒时机、消息可见性、消费者恢复后继续推进消费。
 * 通过条件：延迟唤醒不导致丢消息或卡死，测试返回 0。
 */

#include <galay/cpp/galay-kernel/concurrency/unsafe_channel.h>
#include <galay/cpp/galay-kernel/core/runtime.h>
#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>

using namespace galay::kernel;
using namespace std::chrono_literals;

static std::atomic<bool> g_done{false};
static int g_batch_size = 0;
static int g_sum = 0;

Task<void> batchConsumer(UnsafeChannel<int>* channel) {
    auto batch = co_await channel->recvBatch(8);
    if (batch) {
        g_batch_size = static_cast<int>(batch->size());
        for (int value : *batch) {
            g_sum += value;
        }
    }
    g_done.store(true, std::memory_order_release);
    co_return;
}

Task<void> batchProducer(UnsafeChannel<int>* channel) {
    channel->send(1);
    channel->send(2);
    co_return;
}

int main() {
    Runtime runtime = RuntimeBuilder().ioSchedulerCount(1).computeSchedulerCount(0).build();
    runtime.start();

    auto* scheduler = runtime.getNextIOScheduler();
    if (!scheduler) {
        std::cerr << "[T26] missing IO scheduler\n";
        runtime.stop();
        return 1;
    }

    UnsafeChannel<int> channel{UnsafeChannelWakeMode::Deferred};
    scheduler->schedule(detail::TaskAccess::detachTask(batchConsumer(&channel)));
    scheduler->schedule(detail::TaskAccess::detachTask(batchProducer(&channel)));

    for (int i = 0; i < 40; ++i) {
        if (g_done.load(std::memory_order_acquire)) {
            break;
        }
        std::this_thread::sleep_for(10ms);
    }

    runtime.stop();

    if (!g_done.load(std::memory_order_acquire)) {
        std::cerr << "[T26] consumer timed out\n";
        return 1;
    }

    if (g_batch_size != 2 || g_sum != 3) {
        std::cerr << "[T26] expected one deferred batch with size=2 sum=3, got size="
                  << g_batch_size << " sum=" << g_sum << "\n";
        return 1;
    }

    std::cout << "T26-UnsafeChannelDeferredWake PASS\n";
    return 0;
}
