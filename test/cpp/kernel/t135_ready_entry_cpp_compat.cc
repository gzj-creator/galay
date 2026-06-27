/**
 * @file t135_ready_entry_cpp_compat.cc
 * @brief 验证 ReadyEntry 引入后 C++ Task 调度语义保持兼容。
 */

#include <galay/cpp/galay-kernel/core/runtime.h>
#include <galay/cpp/galay-kernel/core/scheduler_core.h>
#include <galay/cpp/galay-kernel/core/task.h>
#include <galay/cpp/galay-kernel/core/waker.h>

#include <atomic>
#include <chrono>
#include <coroutine>
#include <iostream>
#include <thread>
#include <type_traits>

using namespace galay::kernel;
using namespace std::chrono_literals;

static_assert(std::is_trivially_copyable_v<detail::ReadyEntry>,
              "ReadyEntry must stay a low-cost queue item");
static_assert(sizeof(detail::ReadyEntry) <= sizeof(void*) * 2,
              "ReadyEntry should fit in two machine words");
static_assert(std::is_constructible_v<detail::ReadyEntry, TaskRef&&>,
              "TaskRef must have a fast ReadyEntry conversion path");

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

Task<void> markTask(std::atomic<int>* counter) {
    counter->fetch_add(1, std::memory_order_release);
    co_return;
}

Task<int> valueTask(int value) {
    co_return value * 2;
}

Task<int> parentTask(std::atomic<int>* steps) {
    steps->fetch_add(1, std::memory_order_release);
    auto child = co_await valueTask(21);
    if (!child.has_value()) {
        co_return -1;
    }
    steps->fetch_add(1, std::memory_order_release);
    co_return *child + 1;
}

Task<void> thenStep(std::atomic<int>* sequence, int expected, int next) {
    int observed = sequence->load(std::memory_order_acquire);
    if (observed == expected) {
        sequence->store(next, std::memory_order_release);
    }
    co_return;
}

struct ManualWakeState {
    Waker waker;
    std::atomic<bool> armed{false};
    std::atomic<int> resumed{0};
};

struct ManualSuspendAwaitable {
    ManualWakeState* state;

    bool await_ready() const noexcept { return false; }

    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> handle) noexcept {
        state->waker = Waker(handle);
        state->armed.store(true, std::memory_order_release);
        return true;
    }

    void await_resume() const noexcept {}
};

Task<void> parkedTask(ManualWakeState* state) {
    co_await ManualSuspendAwaitable{state};
    state->resumed.fetch_add(1, std::memory_order_release);
    co_return;
}

TaskRef makeDummyTaskRef() {
    return TaskRef(new TaskState(std::coroutine_handle<>{}), false);
}

bool verifyReadyEntryTaskRefRoundTrip() {
    TaskRef original = makeDummyTaskRef();
    TaskState* const state = original.state();
    detail::ReadyEntry entry(std::move(original));

    if (original.isValid()) {
        std::cerr << "[T135] TaskRef should be moved into ReadyEntry\n";
        return false;
    }
    if (!entry.isCppTask() || entry.state() != state) {
        std::cerr << "[T135] ReadyEntry should carry the original C++ TaskState\n";
        return false;
    }

    TaskRef restored = detail::readyEntryToTaskRef(entry);
    if (restored.state() != state || entry.isValid()) {
        std::cerr << "[T135] ReadyEntry should release ownership back to TaskRef\n";
        return false;
    }
    return true;
}

bool verifySchedulerCoreAcceptsReadyEntryResume() {
    IOSchedulerWorkerState worker;
    SchedulerCore core(worker, 4);
    worker.scheduleLocal(makeDummyTaskRef());

    size_t ready_entries = 0;
    const size_t ran = core.runReadyPass([&](detail::ReadyEntry& entry) {
        if (!entry.isCppTask()) {
            return;
        }
        TaskRef task = detail::readyEntryToTaskRef(entry);
        if (task.isValid()) {
            ++ready_entries;
        }
    });

    if (ran != 1 || ready_entries != 1) {
        std::cerr << "[T135] SchedulerCore should expose ReadyEntry to compatible callbacks\n";
        return false;
    }
    return true;
}

bool verifyRuntimeCppCompatibility() {
    Runtime runtime = RuntimeBuilder()
        .ioSchedulerCount(1)
        .computeSchedulerCount(0)
        .build();

    std::atomic<int> spawned{0};
    auto void_handle = runtime.spawn(markTask(&spawned));
    if (!void_handle.has_value()) {
        std::cerr << "[T135] spawn(Task<void>) failed\n";
        return false;
    }
    auto void_join = void_handle->join();
    if (!void_join.has_value() || spawned.load(std::memory_order_acquire) != 1) {
        std::cerr << "[T135] spawn(Task<void>) did not complete\n";
        runtime.stop();
        return false;
    }

    auto value_handle = runtime.spawn(valueTask(9));
    if (!value_handle.has_value()) {
        std::cerr << "[T135] spawn(Task<int>) failed\n";
        runtime.stop();
        return false;
    }
    auto value = value_handle->join();
    if (!value.has_value() || *value != 18) {
        std::cerr << "[T135] Task<T> result propagation failed\n";
        runtime.stop();
        return false;
    }

    std::atomic<int> parent_steps{0};
    auto parent = runtime.blockOn(parentTask(&parent_steps));
    if (!parent.has_value() || *parent != 43 ||
        parent_steps.load(std::memory_order_acquire) != 2) {
        std::cerr << "[T135] co_await Task<T> parent resume failed\n";
        runtime.stop();
        return false;
    }

    runtime.stop();
    return true;
}

bool verifyThenCompatibility() {
    Runtime runtime = RuntimeBuilder()
        .ioSchedulerCount(0)
        .computeSchedulerCount(1)
        .build();

    std::atomic<int> sequence{0};
    auto result = runtime.blockOn(thenStep(&sequence, 0, 1).then(thenStep(&sequence, 1, 2)));
    runtime.stop();

    if (!result.has_value() || sequence.load(std::memory_order_acquire) != 2) {
        std::cerr << "[T135] then() continuation did not run in order\n";
        return false;
    }
    return true;
}

bool verifyCrossThreadWakeAndCoalescing() {
    Runtime runtime = RuntimeBuilder()
        .ioSchedulerCount(1)
        .computeSchedulerCount(0)
        .build();
    auto started = runtime.start();
    if (!started.has_value()) {
        std::cerr << "[T135] failed to start runtime for cross-thread wake test\n";
        return false;
    }

    auto* scheduler = runtime.getNextIOScheduler();
    if (scheduler == nullptr) {
        std::cerr << "[T135] missing IO scheduler\n";
        return false;
    }

    ManualWakeState state;
    if (!scheduleTask(*scheduler, parkedTask(&state))) {
        std::cerr << "[T135] failed to schedule parked task\n";
        runtime.stop();
        return false;
    }

    if (!waitUntil([&]() { return state.armed.load(std::memory_order_acquire); })) {
        std::cerr << "[T135] parked task did not arm waker\n";
        runtime.stop();
        return false;
    }

    std::thread producer([&]() {
        state.waker.wakeUp();
        state.waker.wakeUp();
        state.waker.wakeUp();
    });
    producer.join();

    const bool resumed_once = waitUntil([&]() {
        return state.resumed.load(std::memory_order_acquire) == 1;
    });
    runtime.stop();

    if (!resumed_once || state.resumed.load(std::memory_order_acquire) != 1) {
        std::cerr << "[T135] cross-thread wake should resume exactly once, got "
                  << state.resumed.load(std::memory_order_acquire) << "\n";
        return false;
    }
    return true;
}

}  // namespace

int main() {
    if (!verifyReadyEntryTaskRefRoundTrip()) {
        return 1;
    }
    if (!verifySchedulerCoreAcceptsReadyEntryResume()) {
        return 1;
    }
    if (!verifyRuntimeCppCompatibility()) {
        return 1;
    }
    if (!verifyThenCompatibility()) {
        return 1;
    }
    if (!verifyCrossThreadWakeAndCoalescing()) {
        return 1;
    }

    std::cout << "T135-ReadyEntryCppCompat PASS\n";
    return 0;
}
