#include "galay-kernel/core/runtime.h"
#include "galay-kernel/common/sleep.hpp"
#include "galay-kernel/concurrency/unsafe_channel.h"

#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>

using namespace galay::kernel;
using namespace std::chrono_literals;

namespace {

std::atomic<int> g_result{0};

Task<void> receiver(UnsafeChannel<int>* channel) {
    auto value = co_await channel->recv();
    if (!value) {
        std::cerr << "receiver failed: " << value.error().message() << "\n";
        g_result.store(-1, std::memory_order_release);
        co_return;
    }

    if (value.value() != 42) {
        std::cerr << "unexpected payload: " << value.value() << "\n";
        g_result.store(-2, std::memory_order_release);
        co_return;
    }

    g_result.store(1, std::memory_order_release);
    co_return;
}

Task<void> sender(UnsafeChannel<int>* channel) {
    co_await galay::kernel::sleep(50ms);
    channel->send(42);
    co_return;
}

}  // namespace

int main() {
    Runtime runtime = RuntimeBuilder().ioSchedulerCount(1).computeSchedulerCount(0).build();
    runtime.start();

    UnsafeChannel<int> channel;
    auto receiver_join = runtime.spawn(receiver(&channel));
    auto sender_join = runtime.spawn(sender(&channel));
    if (!receiver_join || !sender_join) {
        std::cerr << "runtime.spawn failed\n";
        runtime.stop();
        return 1;
    }

    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (g_result.load(std::memory_order_acquire) == 0 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(10ms);
    }

    const int result = g_result.load(std::memory_order_acquire);
    if (result == 1) {
        auto receiver_result = receiver_join->join();
        auto sender_result = sender_join->join();
        if (!receiver_result || !sender_result) {
            std::cerr << "join failed\n";
            runtime.stop();
            return 1;
        }
    }

    runtime.stop();

    if (result != 1) {
        std::cerr << "T56-TaskJoin FAIL result=" << result << "\n";
        return 1;
    }

    std::cout << "T56-TaskJoin PASS\n";
    return 0;
}
