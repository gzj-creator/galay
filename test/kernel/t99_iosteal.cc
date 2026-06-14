/**
 * @file t99_iosteal.cc
 * @brief 用途：验证 Runtime 管理的 IOScheduler 之间能真实发生 work-stealing。
 * 关键覆盖点：空闲 sibling 能偷到偏斜负载；本地仍有 ready 任务时不会先偷 sibling。
 * 通过条件：两个场景都满足断言，测试返回 0。
 */

#include "kernel/common/timer_manager.hpp"
#include "kernel/kernel/runtime.h"
#include "kernel/kernel/task.h"

#if defined(USE_KQUEUE)
#include "kernel/kernel/kqueue_scheduler.h"
using IOSchedulerType = galay::kernel::KqueueScheduler;
static constexpr const char* kBackendName = "kqueue";
#elif defined(USE_EPOLL)
#include "kernel/kernel/epoll_scheduler.h"
using IOSchedulerType = galay::kernel::EpollScheduler;
static constexpr const char* kBackendName = "epoll";
#elif defined(USE_IOURING)
#include "kernel/kernel/uring_scheduler.h"
using IOSchedulerType = galay::kernel::IOUringScheduler;
static constexpr const char* kBackendName = "io_uring";
#else
#error "T99-RuntimeIOWorkStealing requires kqueue, epoll, or io_uring"
#endif

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <thread>

using namespace galay::kernel;
using namespace std::chrono_literals;

namespace {

bool waitUntil(auto&& predicate,
               std::chrono::milliseconds timeout = 1500ms,
               std::chrono::milliseconds step = 1ms) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(step);
    }
    return predicate();
}

void waitForFlag(const std::atomic<bool>& flag) {
    while (!flag.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
}

struct RuntimePair {
    Runtime runtime;
    IOSchedulerType* source = nullptr;
    IOSchedulerType* sibling = nullptr;
};

void startRuntimePair(RuntimePair& pair, uint64_t tick_ns = 1'000'000ULL) {
    auto source = std::make_unique<IOSchedulerType>();
    auto sibling = std::make_unique<IOSchedulerType>();
    source->replaceTimerManager(TimingWheelTimerManager(tick_ns));
    sibling->replaceTimerManager(TimingWheelTimerManager(tick_ns));
    pair.source = source.get();
    pair.sibling = sibling.get();
    pair.runtime.addIOScheduler(std::move(source));
    pair.runtime.addIOScheduler(std::move(sibling));
    pair.runtime.start();

    const bool threads_ready = waitUntil([&]() {
        return pair.source->threadId() != std::thread::id{} &&
               pair.sibling->threadId() != std::thread::id{};
    });
    if (!threads_ready) {
        throw std::runtime_error("scheduler threads did not start in time");
    }
}

struct StealScenarioState {
    std::atomic<bool> release{false};
    std::atomic<int> completed{0};
    std::atomic<int> ran_on_source{0};
    std::atomic<int> ran_on_sibling{0};
    std::atomic<int> unknown_thread{0};
};

Task<void> skewedTask(StealScenarioState* state,
                      IOScheduler* source,
                      IOScheduler* sibling) {
    const auto tid = std::this_thread::get_id();
    if (tid == source->threadId()) {
        state->ran_on_source.fetch_add(1, std::memory_order_relaxed);
    } else if (tid == sibling->threadId()) {
        state->ran_on_sibling.fetch_add(1, std::memory_order_relaxed);
    } else {
        state->unknown_thread.fetch_add(1, std::memory_order_relaxed);
    }

    waitForFlag(state->release);
    state->completed.fetch_add(1, std::memory_order_release);
    co_return;
}

bool runIdleSiblingStealsScenario() {
    constexpr int kTaskCount = 64;
    RuntimePair pair;
    startRuntimePair(pair);
    StealScenarioState state;

    for (int i = 0; i < kTaskCount; ++i) {
        if (!scheduleTask(*pair.source, skewedTask(&state, pair.source, pair.sibling))) {
            std::cerr << "[T99] " << kBackendName
                      << " failed to enqueue skewed task " << i << "\n";
            state.release.store(true, std::memory_order_release);
            pair.runtime.stop();
            return false;
        }
    }

    const bool sibling_started = waitUntil([&]() {
        return state.ran_on_sibling.load(std::memory_order_acquire) > 0;
    }, 2000ms);
    state.release.store(true, std::memory_order_release);
    const bool completed = waitUntil([&]() {
        return state.completed.load(std::memory_order_acquire) == kTaskCount;
    }, 4000ms);

    pair.runtime.stop();

    if (!sibling_started) {
        std::cerr << "[T99] " << kBackendName
                  << " idle sibling never started executing skewed backlog\n";
        return false;
    }

    if (!completed) {
        std::cerr << "[T99] " << kBackendName
                  << " idle sibling steal scenario timed out, completed="
                  << state.completed.load(std::memory_order_acquire) << "/" << kTaskCount << "\n";
        return false;
    }

    if (state.unknown_thread.load(std::memory_order_acquire) != 0) {
        std::cerr << "[T99] " << kBackendName
                  << " observed execution on unknown thread\n";
        return false;
    }

    if (state.ran_on_sibling.load(std::memory_order_acquire) == 0) {
        std::cerr << "[T99] " << kBackendName
                  << " expected idle sibling to steal skewed tasks, but all work stayed on source\n";
        return false;
    }

    return true;
}

struct LocalFirstState {
    int local_target = 0;
    std::atomic<bool> local_done{false};
    std::atomic<bool> release_backlog{false};
    std::atomic<bool> allow_source_finish{false};
    std::atomic<int> local_completed{0};
    std::atomic<bool> stole_before_local_done{false};
    std::atomic<int> sibling_executed_backlog{0};
    std::atomic<int> backlog_completed{0};
    std::atomic<int> unknown_thread{0};
};

Task<void> siblingLocalTask(LocalFirstState* state,
                            IOScheduler* sibling) {
    if (std::this_thread::get_id() != sibling->threadId()) {
        state->unknown_thread.fetch_add(1, std::memory_order_relaxed);
    }

    const int completed =
        state->local_completed.fetch_add(1, std::memory_order_acq_rel) + 1;
    if (completed == state->local_target) {
        state->local_done.store(true, std::memory_order_release);
        state->release_backlog.store(true, std::memory_order_release);
    }
    co_return;
}

Task<void> sourceBacklogTask(LocalFirstState* state,
                             IOScheduler* source,
                             IOScheduler* sibling) {
    const auto tid = std::this_thread::get_id();
    if (tid == sibling->threadId()) {
        state->sibling_executed_backlog.fetch_add(1, std::memory_order_relaxed);
        if (!state->local_done.load(std::memory_order_acquire)) {
            state->stole_before_local_done.store(true, std::memory_order_release);
            state->backlog_completed.fetch_add(1, std::memory_order_release);
            co_return;
        }
        state->backlog_completed.fetch_add(1, std::memory_order_release);
        co_return;
    } else if (tid != source->threadId()) {
        state->unknown_thread.fetch_add(1, std::memory_order_relaxed);
    }

    waitForFlag(state->release_backlog);
    waitForFlag(state->allow_source_finish);
    state->backlog_completed.fetch_add(1, std::memory_order_release);
    co_return;
}

bool runLocalReadyBeatsStealScenario() {
    constexpr int kLocalTasks = 12;
    constexpr int kBacklogTasks = 96;
    RuntimePair pair;
    startRuntimePair(pair);
    LocalFirstState state;
    state.local_target = kLocalTasks;

    for (int i = 0; i < kLocalTasks; ++i) {
        if (!scheduleTask(*pair.sibling, siblingLocalTask(&state, pair.sibling))) {
            std::cerr << "[T99] " << kBackendName
                      << " failed to enqueue sibling local task " << i << "\n";
            state.release_backlog.store(true, std::memory_order_release);
            pair.runtime.stop();
            return false;
        }
    }

    for (int i = 0; i < kBacklogTasks; ++i) {
        if (!scheduleTask(*pair.source, sourceBacklogTask(&state, pair.source, pair.sibling))) {
            std::cerr << "[T99] " << kBackendName
                      << " failed to enqueue source backlog task " << i << "\n";
            state.release_backlog.store(true, std::memory_order_release);
            pair.runtime.stop();
            return false;
        }
    }

    const bool local_done = waitUntil([&]() {
        return state.local_done.load(std::memory_order_acquire);
    }, 2000ms);
    if (!local_done) {
        state.release_backlog.store(true, std::memory_order_release);
        state.allow_source_finish.store(true, std::memory_order_release);
    }
    const bool sibling_stole_after_local = waitUntil([&]() {
        return state.sibling_executed_backlog.load(std::memory_order_acquire) > 0;
    }, 2000ms);
    state.allow_source_finish.store(true, std::memory_order_release);
    const bool backlog_done = waitUntil([&]() {
        return state.backlog_completed.load(std::memory_order_acquire) == kBacklogTasks;
    }, 5000ms);

    pair.runtime.stop();

    if (!local_done) {
        std::cerr << "[T99] " << kBackendName
                  << " sibling local ready tasks did not complete in time\n";
        return false;
    }

    if (!backlog_done) {
        std::cerr << "[T99] " << kBackendName
                  << " backlog scenario timed out, completed="
                  << state.backlog_completed.load(std::memory_order_acquire)
                  << "/" << kBacklogTasks << "\n";
        return false;
    }

    if (state.unknown_thread.load(std::memory_order_acquire) != 0) {
        std::cerr << "[T99] " << kBackendName
                  << " observed backlog work on unknown thread\n";
        return false;
    }

    if (!sibling_stole_after_local) {
        std::cerr << "[T99] " << kBackendName
                  << " sibling never stole source backlog after draining local ready work\n";
        return false;
    }

    if (state.stole_before_local_done.load(std::memory_order_acquire)) {
        std::cerr << "[T99] " << kBackendName
                  << " sibling stole backlog before draining its own local ready chain\n";
        return false;
    }

    return true;
}

}  // namespace

int main() {
    try {
        if (!runIdleSiblingStealsScenario()) {
            return 1;
        }
        if (!runLocalReadyBeatsStealScenario()) {
            return 1;
        }
    } catch (const std::exception& ex) {
        std::cerr << "[T99] unexpected exception: " << ex.what() << "\n";
        return 1;
    }

    std::cout << "T99-RuntimeIOWorkStealing PASS\n";
    return 0;
}
