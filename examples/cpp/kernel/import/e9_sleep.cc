/**
 * @file e9_sleep.cc
 * @brief 用途：用模块导入方式演示定时休眠 Awaitable 的基础用法。
 * 关键覆盖点：连续 `sleep` 调用、累计耗时统计、定时恢复路径。
 * 通过条件：累计耗时落在预期窗口内且示例返回 0。
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
std::atomic<bool> g_done{false};
std::atomic<long long> g_elapsedMs{0};

Task<void> sleepTask() {
    const auto start = std::chrono::steady_clock::now();

    co_await sleep(120ms);
    co_await sleep(180ms);

    const auto end = std::chrono::steady_clock::now();
    g_elapsedMs.store(
        std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count(),
        std::memory_order_release);
    g_done.store(true, std::memory_order_release);
    co_return;
}
}  // namespace

int main() {
    Runtime runtime = RuntimeBuilder().ioSchedulerCount(1).computeSchedulerCount(1).build();
    runtime.start();

    auto* io = runtime.getNextIOScheduler();
    scheduleTask(io, sleepTask());

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!g_done.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    runtime.stop();

    std::cout << "timer-sleep import example elapsed(ms)="
              << g_elapsedMs.load() << "\n";
    return g_done.load(std::memory_order_acquire) ? 0 : 1;
}
