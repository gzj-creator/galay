#include <galay/cpp/galay-kernel/concurrency/async_waiter.h>
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

struct WakeOwnershipState {
    std::atomic<bool> release_sibling{false};
    std::atomic<int> armed{0};
    std::atomic<int> resumed_on_source{0};
    std::atomic<int> resumed_on_sibling{0};
    std::atomic<int> wrong_start_thread{0};
    std::atomic<int> wrong_resume_thread{0};
    std::atomic<int> completed{0};
};

Task<void> notifyWaitersOnSource(std::vector<std::unique_ptr<AsyncWaiter<void>>>* waiters) {
    for (auto& waiter : *waiters) {
        (void)waiter->notify();
    }
    co_return;
}

Task<void> occupySibling(WakeOwnershipState* state) {
    while (!state->release_sibling.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    co_return;
}

Task<void> waiterTask(WakeOwnershipState* state,
                      AsyncWaiter<void>* waiter,
                      IOScheduler* source,
                      IOScheduler* sibling) {
    const auto start_tid = std::this_thread::get_id();
    if (start_tid != source->threadId()) {
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
    if (resume_tid == source->threadId()) {
        state->resumed_on_source.fetch_add(1, std::memory_order_relaxed);
    } else if (resume_tid == sibling->threadId()) {
        state->resumed_on_sibling.fetch_add(1, std::memory_order_relaxed);
    } else {
        state->wrong_resume_thread.fetch_add(1, std::memory_order_relaxed);
    }

    co_await galay::kernel::sleep(10ms);
    state->completed.fetch_add(1, std::memory_order_release);
    co_return;
}

bool runWokenTaskStaysOnOwnerScenario() {
    constexpr int kWaiterCount = 96;

    RuntimePair pair;
    startRuntimePair(pair);
    WakeOwnershipState state;
    std::vector<std::unique_ptr<AsyncWaiter<void>>> waiters;
    waiters.reserve(kWaiterCount);

    if (!scheduleTask(*pair.sibling, occupySibling(&state))) {
        std::cerr << "[T120] " << kBackendName << " failed to occupy sibling scheduler\n";
        pair.runtime.stop();
        return false;
    }

    for (int i = 0; i < kWaiterCount; ++i) {
        waiters.push_back(std::make_unique<AsyncWaiter<void>>());
        if (!scheduleTask(*pair.source,
                          waiterTask(&state, waiters.back().get(), pair.source, pair.sibling))) {
            std::cerr << "[T120] " << kBackendName
                      << " failed to enqueue waiter task " << i << "\n";
            state.release_sibling.store(true, std::memory_order_release);
            pair.runtime.stop();
            return false;
        }
    }

    const bool armed = waitUntil([&]() {
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

    if (!scheduleTask(*pair.source, notifyWaitersOnSource(&waiters))) {
        std::cerr << "[T120] " << kBackendName << " failed to schedule source-thread notifier\n";
        pair.runtime.stop();
        return false;
    }

    const bool resumed = waitUntil([&]() {
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

    const bool completed = waitUntil([&]() {
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

} // namespace

int main() {
    if (!runWokenTaskStaysOnOwnerScenario()) {
        return 1;
    }
    return 0;
}
