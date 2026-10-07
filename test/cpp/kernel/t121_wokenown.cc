#include <galay/cpp/galay-kernel/async/async_waiter.h>
#include <galay/cpp/galay-kernel/common/sleep.hpp>
#include <galay/cpp/galay-kernel/common/timer_manager.hpp>
#include <galay/cpp/galay-kernel/core/runtime.h>
#include <galay/cpp/galay-kernel/core/task.h>

#if defined(USE_KQUEUE)
#include <galay/cpp/galay-kernel/core/kqueue_scheduler.h>
using IOSchedulerType = galay::kernel::KqueueScheduler;
static constexpr const char* kBackendName = "kqueue";
#elif defined(USE_EPOLL)
#include <galay/cpp/galay-kernel/core/epoll_scheduler.h>
using IOSchedulerType = galay::kernel::EpollScheduler;
static constexpr const char* kBackendName = "epoll";
#elif defined(USE_IOURING)
#include <galay/cpp/galay-kernel/core/uring_scheduler.h>
using IOSchedulerType = galay::kernel::IOUringScheduler;
static constexpr const char* kBackendName = "io_uring";
#else
#error "T120-runtime_woken_task_owner_thread requires kqueue, epoll, or io_uring"
#endif

#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

using namespace galay::kernel;
using namespace std::chrono_literals;

namespace {

bool wait_until(auto&& predicate,
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

struct RuntimePair {
    Runtime runtime{RuntimeConfig{.io_scheduler_count = 2, .parallel_scheduler_count = 0}};
    IOSchedulerType* source = nullptr;
    IOSchedulerType* sibling = nullptr;
};

void start_runtime_pair(RuntimePair& pair, uint64_t tick_ns = 1'000'000ULL) {
    const auto initialized = pair.runtime.start();
    if (!initialized) { throw std::runtime_error("runtime initialization failed"); }
    pair.runtime.stop();
    pair.source = pair.runtime.get_io_scheduler(0);
    pair.sibling = pair.runtime.get_io_scheduler(1);
    pair.source->replace_timer_manager(TimingWheelTimerManager(tick_ns));
    pair.sibling->replace_timer_manager(TimingWheelTimerManager(tick_ns));
    const auto started = pair.runtime.start();
    if (!started) { throw std::runtime_error("runtime restart failed"); }

    const bool threads_ready = wait_until([&]() {
        return pair.source->thread_id() != std::thread::id{} &&
               pair.sibling->thread_id() != std::thread::id{};
    });
    if (!threads_ready) {
        throw std::runtime_error("scheduler threads did not start in time");
    }
}

struct WakeOwnershipState {
    std::atomic<bool> release_sibling{false};
    std::atomic<int> armed{0};
    std::atomic<int> resumed_on_source{0};
    std::atomic<int> resumed_on_sibling{0};
    std::atomic<int> wrong_start_thread{0};
    std::atomic<int> wrong_resume_thread{0};
    std::atomic<int> completed{0};
};

Task<void> resume_lifecycle_task(std::atomic<int>* resumed) {
    resumed->fetch_add(1, std::memory_order_release);
    co_return;
}

Task<void> notify_waiters_on_source(std::vector<std::unique_ptr<AsyncWaiter<void>>>* waiters) {
    for (auto& waiter : *waiters) {
        (void)waiter->notify();
    }
    co_return;
}

Task<void> occupy_sibling(WakeOwnershipState* state) {
    while (!state->release_sibling.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    co_return;
}

Task<void> waiter_task(WakeOwnershipState* state,
                      AsyncWaiter<void>* waiter,
                      IOScheduler* source,
                      IOScheduler* sibling) {
    const auto start_tid = std::this_thread::get_id();
    if (start_tid != source->thread_id()) {
        state->wrong_start_thread.fetch_add(1, std::memory_order_relaxed);
    }

    state->armed.fetch_add(1, std::memory_order_release);
    auto result = co_await waiter->wait();
    if (!result) {
        state->wrong_resume_thread.fetch_add(1, std::memory_order_relaxed);
        state->completed.fetch_add(1, std::memory_order_release);
        co_return;
    }

    const auto resume_tid = std::this_thread::get_id();
    if (resume_tid == source->thread_id()) {
        state->resumed_on_source.fetch_add(1, std::memory_order_relaxed);
    } else if (resume_tid == sibling->thread_id()) {
        state->resumed_on_sibling.fetch_add(1, std::memory_order_relaxed);
    } else {
        state->wrong_resume_thread.fetch_add(1, std::memory_order_relaxed);
    }

    co_await galay::kernel::sleep(10ms);
    state->completed.fetch_add(1, std::memory_order_release);
    co_return;
}

bool run_woken_task_stays_on_owner_scenario() {
    constexpr int kWaiterCount = 96;

    RuntimePair pair;
    start_runtime_pair(pair);
    WakeOwnershipState state;
    std::vector<std::unique_ptr<AsyncWaiter<void>>> waiters;
    waiters.reserve(kWaiterCount);

    if (!schedule_task(*pair.sibling, occupy_sibling(&state))) {
        std::cerr << "[T120] " << kBackendName << " failed to occupy sibling scheduler\n";
        pair.runtime.stop();
        return false;
    }

    for (int i = 0; i < kWaiterCount; ++i) {
        waiters.push_back(std::make_unique<AsyncWaiter<void>>());
        if (!schedule_task(*pair.source,
                          waiter_task(&state, waiters.back().get(), pair.source, pair.sibling))) {
            std::cerr << "[T120] " << kBackendName
                      << " failed to enqueue waiter task " << i << "\n";
            state.release_sibling.store(true, std::memory_order_release);
            pair.runtime.stop();
            return false;
        }
    }

    const bool armed = wait_until([&]() {
        return state.armed.load(std::memory_order_acquire) == kWaiterCount;
    }, 3000ms);

    if (!armed) {
        std::cerr << "[T120] " << kBackendName
                  << " waiter tasks did not all arm in time: armed="
                  << state.armed.load(std::memory_order_acquire) << "/" << kWaiterCount << "\n";
        state.release_sibling.store(true, std::memory_order_release);
        pair.runtime.stop();
        return false;
    }

    state.release_sibling.store(true, std::memory_order_release);

    if (!schedule_task(*pair.source, notify_waiters_on_source(&waiters))) {
        std::cerr << "[T120] " << kBackendName << " failed to schedule source-thread notifier\n";
        pair.runtime.stop();
        return false;
    }

    const bool resumed = wait_until([&]() {
        return state.resumed_on_source.load(std::memory_order_acquire) +
               state.resumed_on_sibling.load(std::memory_order_acquire) == kWaiterCount;
    }, 4000ms);

    if (!resumed) {
        std::cerr << "[T120] " << kBackendName
                  << " resumed tasks did not all enter post-wake state in time: source="
                  << state.resumed_on_source.load(std::memory_order_acquire)
                  << " sibling=" << state.resumed_on_sibling.load(std::memory_order_acquire)
                  << " total=" << kWaiterCount << "\n";
        pair.runtime.stop();
        return false;
    }

    const bool completed = wait_until([&]() {
        return state.completed.load(std::memory_order_acquire) == kWaiterCount;
    }, 4000ms);

    pair.runtime.stop();

    if (!completed) {
        std::cerr << "[T120] " << kBackendName
                  << " resumed tasks did not complete in time: completed="
                  << state.completed.load(std::memory_order_acquire) << "/" << kWaiterCount << "\n";
        return false;
    }

    if (state.wrong_start_thread.load(std::memory_order_acquire) != 0) {
        std::cerr << "[T120] " << kBackendName
                  << " waiter tasks started on non-owner thread\n";
        return false;
    }

    if (state.wrong_resume_thread.load(std::memory_order_acquire) != 0) {
        std::cerr << "[T120] " << kBackendName
                  << " waiter tasks resumed on unknown thread or with error\n";
        return false;
    }

    if (state.resumed_on_sibling.load(std::memory_order_acquire) != 0) {
        std::cerr << "[T120] " << kBackendName
                  << " owner-woken tasks were stolen by sibling scheduler: sibling_resumes="
                  << state.resumed_on_sibling.load(std::memory_order_acquire) << "\n";
        return false;
    }

    if (state.resumed_on_source.load(std::memory_order_acquire) != kWaiterCount) {
        std::cerr << "[T120] " << kBackendName
                  << " expected all owner-woken tasks to resume on source scheduler: source_resumes="
                  << state.resumed_on_source.load(std::memory_order_acquire) << "/" << kWaiterCount << "\n";
        return false;
    }

    return true;
}

bool run_resume_admission_lifecycle_scenario() {
    IOSchedulerType scheduler;
    std::atomic<int> resumed{0};

    const bool accepted_before_start = scheduler.schedule_resume(
        detail::TaskAccess::detach_task(resume_lifecycle_task(&resumed)));

    const auto started = scheduler.start();
    if (!started.has_value()) {
        std::cerr << "[T120] " << kBackendName
                  << " failed to start lifecycle scheduler: "
                  << started.error().message() << "\n";
        return false;
    }

    const bool accepted_while_running = scheduler.schedule_resume(
        detail::TaskAccess::detach_task(resume_lifecycle_task(&resumed)));
    scheduler.stop();

    const bool accepted_after_stop = scheduler.schedule_resume(
        detail::TaskAccess::detach_task(resume_lifecycle_task(&resumed)));
    const auto restarted = scheduler.start();
    if (!restarted.has_value()) {
        std::cerr << "[T120] " << kBackendName
                  << " failed to restart lifecycle scheduler: "
                  << restarted.error().message() << "\n";
        return false;
    }
    scheduler.stop();

    const int resumed_count = resumed.load(std::memory_order_acquire);
    if (accepted_before_start || !accepted_while_running ||
        accepted_after_stop || resumed_count != 1) {
        std::cerr << "[T120] " << kBackendName
                  << " resume lifecycle mismatch: before_start="
                  << accepted_before_start
                  << " running=" << accepted_while_running
                  << " after_stop=" << accepted_after_stop
                  << " resumed=" << resumed_count << "\n";
        return false;
    }
    return true;
}

} // namespace

int main() {
    if (!run_resume_admission_lifecycle_scenario()) {
        return 1;
    }
    if (!run_woken_task_stays_on_owner_scenario()) {
        return 1;
    }
    return 0;
}
