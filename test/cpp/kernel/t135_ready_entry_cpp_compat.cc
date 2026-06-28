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

static_assert(sizeof(detail::ReadyEntry) <= sizeof(void*) * 2,
              "ReadyEntry should fit in two machine words");
static_assert(std::is_constructible_v<detail::ReadyEntry, TaskRef&&>,
              "TaskRef must have a fast ReadyEntry conversion path");
static_assert(!std::is_copy_constructible_v<detail::ReadyEntry>,
              "ReadyEntry owns a queued reference and must not be copyable");
static_assert(std::is_nothrow_move_constructible_v<detail::ReadyEntry>,
              "ReadyEntry must be cheaply movable across queues");

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
    std::thread::id resumed_thread;
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
    state->resumed_thread = std::this_thread::get_id();
    state->resumed.fetch_add(1, std::memory_order_release);
    co_return;
}

TaskRef makeDummyTaskRef() {
    return TaskRef(new TaskState(std::coroutine_handle<>{}), false);
}

class NullScheduler final : public Scheduler {
public:
    std::expected<void, IOError> start() override { return {}; }
    void stop() override {}
    bool schedule(TaskRef) override { return false; }
    bool scheduleDeferred(TaskRef) override { return false; }
    bool scheduleImmediately(TaskRef) override { return false; }
    bool addTimer(Timer::ptr) override { return false; }
    SchedulerType type() override { return kComputeScheduler; }
};

struct FakeCoroState {
    detail::ReadyEntryCoroHeader header;
    Scheduler* owner = nullptr;
    bool owner_only = false;
    std::atomic<int>* resumed = nullptr;
    std::atomic<int>* released = nullptr;
};

Scheduler* fakeOwnerScheduler(void* state) noexcept {
    return static_cast<FakeCoroState*>(state)->owner;
}

bool fakeResumeOwnerOnly(void* state) noexcept {
    auto* fake = static_cast<FakeCoroState*>(state);
    return fake->owner_only;
}

bool fakeResume(void* state) noexcept {
    auto* fake = static_cast<FakeCoroState*>(state);
    if (fake->resumed != nullptr) {
        fake->resumed->fetch_add(1, std::memory_order_release);
    }
    return true;
}

void fakeRelease(void* state) noexcept {
    auto* fake = static_cast<FakeCoroState*>(state);
    if (fake->released != nullptr) {
        fake->released->fetch_add(1, std::memory_order_release);
    }
}

constexpr detail::ReadyEntryHooks kFakeCoroHooks{
    .owner_scheduler = fakeOwnerScheduler,
    .resume_owner_only = fakeResumeOwnerOnly,
    .resume = fakeResume,
    .release = fakeRelease,
};

detail::ReadyEntry makeFakeCoroEntry(FakeCoroState* state) {
    state->header.hooks = &kFakeCoroHooks;
    return detail::ReadyEntry(detail::ReadyEntryKind::CCoroutine, state);
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

bool verifyCCoroutineReadyEntryHooks() {
    NullScheduler owner;
    std::atomic<int> resumed{0};
    std::atomic<int> released{0};
    FakeCoroState state{
        .owner = &owner,
        .owner_only = true,
        .resumed = &resumed,
        .released = &released,
    };

    detail::ReadyEntry entry = makeFakeCoroEntry(&state);
    if (!entry.isValid() || entry.kind() != detail::ReadyEntryKind::CCoroutine) {
        std::cerr << "[T135] C coroutine ReadyEntry should be valid\n";
        return false;
    }
    if (detail::readyEntryScheduler(entry) != &owner) {
        std::cerr << "[T135] C coroutine ReadyEntry should expose owner scheduler\n";
        return false;
    }
    if (!detail::readyEntryResumeOwnerOnly(entry)) {
        std::cerr << "[T135] C coroutine ReadyEntry should expose owner-only resume\n";
        return false;
    }
    if (!detail::resumeReadyEntry(entry) ||
        resumed.load(std::memory_order_acquire) != 1 ||
        released.load(std::memory_order_acquire) != 1 ||
        entry.isValid()) {
        std::cerr << "[T135] C coroutine ReadyEntry should resume through hook and release ownership\n";
        return false;
    }

    return true;
}

bool verifyCCoroutineScheduleRejectKeepsEntry() {
    std::atomic<int> released{0};
    FakeCoroState state{.released = &released};
    detail::ReadyEntry entry = makeFakeCoroEntry(&state);

    if (detail::scheduleReadyEntry(entry) || !entry.isValid() ||
        released.load(std::memory_order_acquire) != 0) {
        std::cerr << "[T135] unsupported C coroutine schedule should reject without release\n";
        return false;
    }

    detail::releaseReadyEntry(entry);
    if (released.load(std::memory_order_acquire) != 1) {
        std::cerr << "[T135] rejected C coroutine entry should remain owned by caller\n";
        return false;
    }
    return true;
}

bool verifyCppScheduleRejectKeepsEntry() {
    NullScheduler rejecting_scheduler;
    TaskRef task = makeDummyTaskRef();
    TaskState* const state = task.state();
    detail::setTaskScheduler(task, &rejecting_scheduler);
    detail::ReadyEntry entry(std::move(task));

    if (detail::scheduleReadyEntry(entry) || !entry.isValid() ||
        entry.taskState() != state) {
        std::cerr << "[T135] rejected C++ schedule should keep ReadyEntry owned by caller\n";
        return false;
    }

    TaskRef restored = detail::readyEntryToTaskRef(entry);
    if (!restored.isValid() || restored.state() != state ||
        restored.belongScheduler() != &rejecting_scheduler) {
        std::cerr << "[T135] rejected C++ schedule should remain recoverable as TaskRef\n";
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

bool verifyOwnerOnlyCCoroutineEntryIsReturnedToCaller() {
    std::atomic<int> released{0};
    FakeCoroState state{
        .owner_only = true,
        .released = &released,
    };
    detail::ReadyEntry entry = makeFakeCoroEntry(&state);
    ChaseLevTaskRing ring;

    if (!ring.push_back(entry) || entry.isValid()) {
        std::cerr << "[T135] failed to queue fake C coroutine entry\n";
        return false;
    }

    detail::ReadyEntry stolen;
    if (ring.steal_front(stolen) || !stolen.isValid()) {
        std::cerr << "[T135] owner-only C coroutine entry should be returned to caller after CAS\n";
        detail::releaseReadyEntry(stolen);
        return false;
    }
    if (ring.size() != 0 || released.load(std::memory_order_acquire) != 0) {
        std::cerr << "[T135] ring should not pre-CAS peek or release owner-only C coroutine entry\n";
        return false;
    }

    detail::releaseReadyEntry(stolen);
    if (released.load(std::memory_order_acquire) != 1) {
        std::cerr << "[T135] returned owner-only C coroutine entry should release exactly once\n";
        return false;
    }
    return true;
}

bool verifyWorkerRequeuesOwnerOnlyEntryThroughInjectQueue() {
    std::atomic<int> released{0};
    FakeCoroState state{
        .owner_only = true,
        .released = &released,
    };
    detail::ReadyEntry entry = makeFakeCoroEntry(&state);
    IOSchedulerWorkerState worker;

    if (!worker.local_ring.push_back(entry)) {
        std::cerr << "[T135] failed to queue worker owner-only probe\n";
        return false;
    }

    detail::ReadyEntry stolen;
    if (worker.stealFront(stolen) || stolen.isValid()) {
        std::cerr << "[T135] worker owner-only probe must not be exposed to stealer\n";
        detail::releaseReadyEntry(stolen);
        return false;
    }
    if (!worker.hasPendingInjected() || worker.local_ring.size() != 0 ||
        released.load(std::memory_order_acquire) != 0) {
        std::cerr << "[T135] worker should requeue owner-only entry through inject queue\n";
        return false;
    }

    if (worker.drainInjected() != 1 || !worker.popNext(stolen)) {
        std::cerr << "[T135] owner should recover worker-requeued owner-only entry\n";
        return false;
    }
    detail::releaseReadyEntry(stolen);
    return released.load(std::memory_order_acquire) == 1;
}

bool verifyPendingReadyEntriesReleaseOnWorkerDestroy() {
    std::atomic<int> released{0};
    FakeCoroState ring_state{.released = &released};
    FakeCoroState lifo_state{.released = &released};
    FakeCoroState injected_state{.released = &released};
    FakeCoroState buffer_state{.released = &released};

    {
        IOSchedulerWorkerState worker(2);
        worker.scheduleLocal(makeFakeCoroEntry(&ring_state));
        worker.scheduleLocal(makeFakeCoroEntry(&lifo_state));
        worker.scheduleInjected(makeFakeCoroEntry(&injected_state));
        worker.ready_inject_buffer[0] = makeFakeCoroEntry(&buffer_state);
    }

    if (released.load(std::memory_order_acquire) != 4) {
        std::cerr << "[T135] pending ReadyEntry cleanup should release LIFO/ring/inject/buffer entries, released="
                  << released.load(std::memory_order_acquire) << "\n";
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
    if (state.resumed_thread != scheduler->threadId()) {
        std::cerr << "[T135] cross-thread wake resumed on non-owner scheduler thread\n";
        return false;
    }
    return true;
}

}  // namespace

int main() {
    if (!verifyReadyEntryTaskRefRoundTrip()) {
        return 1;
    }
    if (!verifyCCoroutineReadyEntryHooks()) {
        return 1;
    }
    if (!verifyCCoroutineScheduleRejectKeepsEntry()) {
        return 1;
    }
    if (!verifyCppScheduleRejectKeepsEntry()) {
        return 1;
    }
    if (!verifySchedulerCoreAcceptsReadyEntryResume()) {
        return 1;
    }
    if (!verifyOwnerOnlyCCoroutineEntryIsReturnedToCaller()) {
        return 1;
    }
    if (!verifyWorkerRequeuesOwnerOnlyEntryThroughInjectQueue()) {
        return 1;
    }
    if (!verifyPendingReadyEntriesReleaseOnWorkerDestroy()) {
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
