/**
 * @file t85_blkpool.cc
 * @brief 用途：验证 `BlockingExecutor` 使用受限弹性线程池执行阻塞任务。
 * 关键覆盖点：最大 worker 数上限、任务排队、线程池复用语义。
 * 通过条件：`max_workers = 2` 时，3 个阻塞任务峰值并发不超过 2，且总耗时体现两波执行。
 */

#include <galay/cpp/galay-kernel/core/blocking_executor.h>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <iostream>
#include <mutex>
#include <thread>

using namespace galay::kernel;

namespace
{

void updatePeak(std::atomic<int>& peak, int active)
{
    int observed = peak.load(std::memory_order_relaxed);
    while (observed < active &&
           !peak.compare_exchange_weak(observed, active, std::memory_order_relaxed)) {
    }
}

} // namespace

int main()
{
    BlockingExecutor executor(0, 2, std::chrono::milliseconds(1000));

    std::atomic<int> active{0};
    std::atomic<int> peak{0};
    std::atomic<int> done{0};
    std::mutex mutex;
    std::condition_variable cv;

    const auto start = std::chrono::steady_clock::now();

    for (int i = 0; i < 3; ++i) {
        executor.submit([&]() {
            const int current = active.fetch_add(1, std::memory_order_relaxed) + 1;
            updatePeak(peak, current);

            std::this_thread::sleep_for(std::chrono::milliseconds(100));

            active.fetch_sub(1, std::memory_order_relaxed);
            done.fetch_add(1, std::memory_order_release);
            cv.notify_one();
        });
    }

    {
        std::unique_lock<std::mutex> lock(mutex);
        cv.wait(lock, [&]() { return done.load(std::memory_order_acquire) == 3; });
    }

    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();

    assert(peak.load(std::memory_order_relaxed) == 2 &&
           "BlockingExecutor should cap concurrent workers at max_workers");
    assert(elapsed_ms >= 190 &&
           "BlockingExecutor should queue overflow tasks once max_workers is reached");
    assert(elapsed_ms < 320 && "BlockingExecutor should complete 3x100ms tasks in two waves");

    std::cout << "T85-BlockingExecutorPool PASS\n";
    return 0;
}
