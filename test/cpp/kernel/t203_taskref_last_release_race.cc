/**
 * @file t203_taskref_last_release_race.cc
 * @brief 多个拥有引用同时退出时只销毁一次，并向最后析构者发布各线程先前的写入。
 */

#include <galay/cpp/galay-kernel/core/task.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <iostream>
#include <thread>
#include <utility>

namespace {

using galay::kernel::Task;
using galay::kernel::TaskRef;

/** @brief 在 frame 最后销毁时检查所有退出线程的非原子写入是否已经可见。 */
template <std::size_t Workers>
struct LifetimeProbe {
    std::array<int, Workers>& writes;
    std::atomic<int>* destroyed;
    std::atomic<bool>& failed;

    LifetimeProbe(std::array<int, Workers>& values, std::atomic<int>& count,
                  std::atomic<bool>& error) noexcept
        : writes(values), destroyed(&count), failed(error) {}
    LifetimeProbe(const LifetimeProbe&) = delete;
    LifetimeProbe(LifetimeProbe&& other) noexcept
        : writes(other.writes), destroyed(std::exchange(other.destroyed, nullptr)),
          failed(other.failed) {}

    ~LifetimeProbe()
    {
        if (destroyed == nullptr) {
            return;
        }
        for (std::size_t index = 0; index < Workers; ++index) {
            if (writes[index] != static_cast<int>(index + 1)) {
                failed.store(true, std::memory_order_relaxed);
            }
        }
        // 只统计析构次数，之前的计数没有消费方。
        (void)destroyed->fetch_add(1, std::memory_order_relaxed);
    }
};

/** @brief 未启动的 frame 持有检查器，最后一个 TaskRef 负责销毁。 */
template <std::size_t Workers>
Task<void> held_task(LifetimeProbe<Workers> probe)
{
    // 检查器的资源归属 frame，测试刻意不启动协程。
    (void)probe;
    co_return;
}

/** @brief 同时释放多个最后引用，检查唯一销毁和 release/acquire 发布契约。 */
template <std::size_t Workers>
bool verify_concurrent_last_release()
{
    constexpr std::size_t kRounds = 64;
    for (std::size_t round = 0; round < kRounds; ++round) {
        std::array<int, Workers> writes{};
        std::atomic<int> destroyed{0};
        std::atomic<bool> failed{false};
        std::atomic<bool> start{false};
        auto task = held_task(LifetimeProbe<Workers>(writes, destroyed, failed));
        TaskRef owner = galay::kernel::detail::TaskAccess::detach_task(std::move(task));
        if (!owner.is_valid()) {
            std::cerr << "[T203] failed to create a task\n";
            return false;
        }
        std::array<std::thread, Workers> threads;
        for (std::size_t index = 0; index < Workers; ++index) {
            TaskRef ref = owner;
            threads[index] = std::thread([&, index, ref = std::move(ref)]() mutable {
                while (!start.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }
                writes[index] = static_cast<int>(index + 1);
                ref = TaskRef{};
            });
        }
        owner = TaskRef{};
        start.store(true, std::memory_order_release);
        for (auto& thread : threads) {
            thread.join();
        }
        if (destroyed.load(std::memory_order_relaxed) != 1 ||
            failed.load(std::memory_order_relaxed)) {
            std::cerr << "[T203] last release lost writes or destroyed the frame more than once\n";
            return false;
        }
    }
    return true;
}

} // namespace

int main()
{
    return verify_concurrent_last_release<2>() &&
        verify_concurrent_last_release<4>() && verify_concurrent_last_release<8>() ? 0 : 1;
}
