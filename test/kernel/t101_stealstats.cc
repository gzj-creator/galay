/**
 * @file t101_stealstats.cc
 * @brief 用途：验证 Runtime 能暴露 IOScheduler stealing 统计。
 * 关键覆盖点：steal attempts/successes 快照、skewed load 下成功计数增长。
 * 通过条件：统计结构可读取，且至少一个 scheduler 记录到成功 stealing。
 */

#include "galay-kernel/common/timer_manager.hpp"
#include "galay-kernel/core/runtime.h"
#include "galay-kernel/core/task.h"
#include "test/sched_access.h"

#if defined(USE_KQUEUE)
#include "galay-kernel/core/kqueue_scheduler.h"
using IOSchedulerType = galay::kernel::KqueueScheduler;
#elif defined(USE_EPOLL)
#include "galay-kernel/core/epoll_scheduler.h"
using IOSchedulerType = galay::kernel::EpollScheduler;
#elif defined(USE_IOURING)
#include "galay-kernel/core/uring_scheduler.h"
using IOSchedulerType = galay::kernel::IOUringScheduler;
#else
#error "T101-RuntimeIOStealStats requires kqueue, epoll, or io_uring"
#endif

#include <iostream>
#include <memory>
#include <span>
#include <vector>

using namespace galay::kernel;

namespace {

TaskRef makeStealableTask(IOScheduler* owner) {
    auto* state = new TaskState(std::coroutine_handle<>{});
    state->m_scheduler = owner;
    return TaskRef(state, false);
}

bool runStatsScenario() {
    constexpr int kTaskCount = 16;

    Runtime runtime;
    auto source = std::make_unique<IOSchedulerType>();
    auto sibling = std::make_unique<IOSchedulerType>();
    auto* source_ptr = source.get();
    auto* sibling_ptr = sibling.get();
    runtime.addIOScheduler(std::move(source));
    runtime.addIOScheduler(std::move(sibling));

    std::vector<IOScheduler*> siblings{source_ptr, sibling_ptr};
    std::span<IOScheduler* const> sibling_span{siblings.data(), siblings.size()};
    source_ptr->configureStealDomain(sibling_span, 0);
    sibling_ptr->configureStealDomain(sibling_span, 1);

    auto& source_worker = SchedulerTestAccess::worker(*source_ptr);
    auto& sibling_worker = SchedulerTestAccess::worker(*sibling_ptr);
    for (int i = 0; i < kTaskCount; ++i) {
        auto task = makeStealableTask(source_ptr);
        if (!source_worker.local_ring.push_back(std::move(task))) {
            std::cerr << "[T101] failed to seed source worker backlog " << i << "\n";
            return false;
        }
    }

    if (!sibling_worker.trySteal()) {
        std::cerr << "[T101] sibling worker failed to steal seeded backlog\n";
        return false;
    }

    const auto stats = runtime.stats();
    if (stats.io_schedulers.size() != 2) {
        std::cerr << "[T101] expected two io scheduler stats, actual="
                  << stats.io_schedulers.size() << "\n";
        return false;
    }

    uint64_t total_attempts = 0;
    uint64_t total_successes = 0;
    for (const auto& scheduler : stats.io_schedulers) {
        total_attempts += scheduler.steal_attempts;
        total_successes += scheduler.steal_successes;
    }

    if (total_attempts == 0) {
        std::cerr << "[T101] expected at least one steal attempt in stats\n";
        return false;
    }

    if (total_successes == 0) {
        std::cerr << "[T101] expected at least one successful steal in stats\n";
        return false;
    }

    return true;
}

}  // namespace

int main() {
    if (!runStatsScenario()) {
        return 1;
    }

    std::cout << "T101-RuntimeIOStealStats PASS\n";
    return 0;
}
