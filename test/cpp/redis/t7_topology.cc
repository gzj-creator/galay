#include <array>
#include <chrono>
#include <iostream>
#include <thread>

#include <galay/cpp/galay-redis/async/topology_client.h>
#include "../common/sched_access.h"

using namespace galay::kernel;
using namespace galay::redis;

namespace {

bool checkLazyTasks(IOScheduler& scheduler, std::array<Task<RedisCommandResult>, 3>& tasks)
{
    auto& worker = SchedulerTestAccess::worker(scheduler);
    if (worker.hasLocalWork() || worker.hasPendingInjected()) {
        std::cerr << "Creating refresh tasks must not submit work\n";
        return false;
    }
    std::array<TaskRef, 3> refs;
    for (size_t i = 0; i < tasks.size(); ++i) {
        refs[i] = galay::kernel::detail::TaskAccess::taskRef(tasks[i]);
        if (!refs[i].isValid() || refs[i].belongScheduler() != nullptr ||
            refs[i].state()->m_done.load(std::memory_order_acquire)) {
            std::cerr << "Refresh task must remain unbound and suspended until submission\n";
            return false;
        }
        if (!scheduleTask(scheduler, std::move(tasks[i]))) {
            return false;
        }
    }
    if (worker.injected_outstanding.load(std::memory_order_acquire) != tasks.size()) {
        std::cerr << "Expected exactly three submitted refresh tasks\n";
        return false;
    }
    const auto started = scheduler.start();
    if (!started) { return false; }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    bool completed = false;
    do {
        completed = true;
        for (const auto& task : refs) {
            completed = completed && task.state()->m_done.load(std::memory_order_acquire);
        }
        if (completed) { break; }
        std::this_thread::yield();
    } while (std::chrono::steady_clock::now() < deadline);
    scheduler.stop();
    if (!completed) { std::cerr << "Refresh tasks did not finish on the real IO scheduler\n"; }
    return completed;
}

} // namespace

int main()
{
    {
        IOScheduler scheduler;
        auto client = RedisMasterSlaveClientBuilder().scheduler(&scheduler).build();
        std::array tasks{client.refreshFromSentinel(), client.refreshFromSentinel(),
                         client.refreshFromSentinel()};
        if (!checkLazyTasks(scheduler, tasks)) { return 1; }
    }
    {
        IOScheduler scheduler;
        auto client = RedisClusterClientBuilder().scheduler(&scheduler).build();
        std::array tasks{client.refreshSlots(), client.refreshSlots(), client.refreshSlots()};
        if (!checkLazyTasks(scheduler, tasks)) { return 1; }
    }
    std::cout << "Topology task laziness PASS\n";
    return 0;
}
