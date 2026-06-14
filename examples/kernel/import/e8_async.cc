/**
 * @file e8_async.cc
 * @brief 用途：用模块导入方式演示 `AsyncMutex` 与 `AsyncWaiter` 的协作用法。
 * 关键覆盖点：异步互斥保护、计算任务通知、跨调度器等待与结果读取。
 * 通过条件：计数结果和计算结果都符合预期，示例返回 0。
 */

import galay.kernel;

#include <coroutine>
#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>

using namespace galay::kernel;
using namespace std::chrono_literals;

namespace {
AsyncMutex g_mutex;
int g_counter = 0;

std::atomic<int> g_worker_done{0};
std::atomic<bool> g_wait_done{false};
std::atomic<int> g_compute_value{0};

Task<void> guardedWorker(int iterations) {
    for (int i = 0; i < iterations; ++i) {
        auto locked = co_await g_mutex.lock().timeout(200ms);
        if (!locked) {
            continue;
        }

        ++g_counter;
        g_mutex.unlock();
        co_yield true;
    }

    g_worker_done.fetch_add(1, std::memory_order_relaxed);
    co_return;
}

Task<void> computeTask(AsyncWaiter<int>* waiter) {
    int sum = 0;
    for (int i = 1; i <= 1000; ++i) {
        sum += i;
    }
    waiter->notify(sum);
    co_return;
}

Task<void> waitComputeResult(AsyncWaiter<int>* waiter) {
    auto result = co_await waiter->wait().timeout(1s);
    if (result) {
        g_compute_value.store(result.value(), std::memory_order_release);
        g_wait_done.store(true, std::memory_order_release);
    }
    co_return;
}
}  // namespace

int main() {
    constexpr int kIterations = 500;

    Runtime runtime = RuntimeBuilder().ioSchedulerCount(1).computeSchedulerCount(1).build();
    runtime.start();

    auto* io = runtime.getNextIOScheduler();
    auto* compute = runtime.getNextComputeScheduler();

    AsyncWaiter<int> waiter;

    scheduleTask(io, guardedWorker(kIterations));
    scheduleTask(io, guardedWorker(kIterations));
    scheduleTask(io, waitComputeResult(&waiter));
    scheduleTask(compute, computeTask(&waiter));

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while ((g_worker_done.load(std::memory_order_acquire) < 2 ||
            !g_wait_done.load(std::memory_order_acquire)) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    runtime.stop();

    std::cout << "async-sync import example counter=" << g_counter
              << ", computeResult=" << g_compute_value.load() << "\n";
    return (g_counter == 2 * kIterations &&
            g_wait_done.load(std::memory_order_acquire)) ? 0 : 1;
}
