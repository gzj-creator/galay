/**
 * @file t99_iosteal.cc
 * @brief 用途：验证 Runtime 管理的具体 IOScheduler 不跨 sibling 窃取 IO task。
 *
 * 关键覆盖点：空闲 sibling 不执行绑定到 source scheduler 的 IO 协程，避免
 * reactor 注册/删除在错误线程发生。
 * 通过条件：偏斜负载期间 sibling 执行数保持 0，释放后所有任务在 source 完成。
 */

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
#error "T99-RuntimeIONoWorkStealing requires kqueue, epoll, or io_uring"
#endif

#include <atomic>
#include <chrono>
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

struct NoStealScenarioState {
    std::atomic<bool> release{false};
    std::atomic<int> entered{0};
    std::atomic<int> completed{0};
    std::atomic<int> ran_on_source{0};
    std::atomic<int> ran_on_sibling{0};
    std::atomic<int> unknown_thread{0};
};

Task<void> sourceOwnedTask(NoStealScenarioState* state,
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

    state->entered.fetch_add(1, std::memory_order_release);
    waitForFlag(state->release);
    state->completed.fetch_add(1, std::memory_order_release);
    co_return;
}

bool runConcreteIOSchedulerDoesNotStealScenario() {
    constexpr int kTaskCount = 64;
    RuntimePair pair;
    startRuntimePair(pair);
    NoStealScenarioState state;

    for (int i = 0; i < kTaskCount; ++i) {
        if (!scheduleTask(*pair.source, sourceOwnedTask(&state, pair.source, pair.sibling))) {
            std::cerr << "[T99] " << kBackendName
                      << " failed to enqueue source task " << i << "\n";
            state.release.store(true, std::memory_order_release);
            pair.runtime.stop();
            return false;
        }
    }

    const bool first_entered = waitUntil([&]() {
        return state.entered.load(std::memory_order_acquire) > 0;
    }, 2000ms);
    const bool sibling_ran = waitUntil([&]() {
        return state.ran_on_sibling.load(std::memory_order_acquire) > 0;
    }, 500ms);

    state.release.store(true, std::memory_order_release);
    const bool completed = waitUntil([&]() {
        return state.completed.load(std::memory_order_acquire) == kTaskCount;
    }, 4000ms);

    pair.runtime.stop();

    if (!first_entered) {
        std::cerr << "[T99] " << kBackendName
                  << " source task did not start in time\n";
        return false;
    }

    if (sibling_ran || state.ran_on_sibling.load(std::memory_order_acquire) != 0) {
        std::cerr << "[T99] " << kBackendName
                  << " sibling executed source-owned IO task: sibling_count="
                  << state.ran_on_sibling.load(std::memory_order_acquire) << "\n";
        return false;
    }

    if (!completed) {
        std::cerr << "[T99] " << kBackendName
                  << " no-steal scenario timed out, completed="
                  << state.completed.load(std::memory_order_acquire) << "/" << kTaskCount << "\n";
        return false;
    }

    if (state.unknown_thread.load(std::memory_order_acquire) != 0) {
        std::cerr << "[T99] " << kBackendName
                  << " observed execution on unknown thread\n";
        return false;
    }

    return true;
}

}  // namespace

int main() {
    try {
        if (!runConcreteIOSchedulerDoesNotStealScenario()) {
            return 1;
        }
    } catch (const std::exception& ex) {
        std::cerr << "[T99] unexpected exception: " << ex.what() << "\n";
        return 1;
    }

    std::cout << "T99-RuntimeIONoWorkStealing PASS\n";
    return 0;
}
