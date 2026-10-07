/**
 * @file t135_ready_entry_cpp_compat.cc
 * @brief 验证 ReadyEntry 引入后 C++ Task 调度语义保持兼容。
 */

#include <galay/cpp/galay-kernel/core/runtime.h>
#include "test/cpp/common/scheduler_test_adapter.h"
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

Task<void> mark_task(std::atomic<int>* counter) {
    counter->fetch_add(1, std::memory_order_release);
    co_return;
}

Task<int> value_task(int value) {
    co_return value * 2;
}

Task<int> parent_task(std::atomic<int>* steps) {
    steps->fetch_add(1, std::memory_order_release);
    auto child = co_await value_task(21);
    if (!child.has_value()) {
        co_return -1;
    }
    steps->fetch_add(1, std::memory_order_release);
    co_return *child + 1;
}

Task<void> then_step(std::atomic<int>* sequence, int expected, int next) {
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

Task<void> parked_task(ManualWakeState* state) {
    co_await ManualSuspendAwaitable{state};
    state->resumed_thread = std::this_thread::get_id();
    state->resumed.fetch_add(1, std::memory_order_release);
    co_return;
}

TaskRef make_dummy_task_ref() {
    return TaskRef(new TaskState(std::coroutine_handle<>{}), false);
}

class NullScheduler final : public detail::SchedulerTestAdapter<NullScheduler> {
public:
    std::expected<void, IOError> start() { return {}; }
    void stop() {}
    bool schedule(TaskRef) noexcept { return false; }
    bool schedule_resume(TaskRef) noexcept { return false; }
    bool schedule_deferred(TaskRef) noexcept { return false; }
    bool schedule_immediately(TaskRef) noexcept { return false; }
    bool add_timer(Timer::ptr) { return false; }
    SchedulerType type() { return kParallelScheduler; }
};

struct FakeCoroState {
    detail::ReadyEntryCoroHeader header;
    Scheduler* owner = nullptr;
    bool owner_only = false;
    std::atomic<int>* resumed = nullptr;
    std::atomic<int>* released = nullptr;
};

Scheduler* fake_owner_scheduler(void* state) noexcept {
    return static_cast<FakeCoroState*>(state)->owner;
}

bool fake_resume_owner_only(void* state) noexcept {
    auto* fake = static_cast<FakeCoroState*>(state);
    return fake->owner_only;
}

bool fake_resume(void* state) noexcept {
    auto* fake = static_cast<FakeCoroState*>(state);
    if (fake->resumed != nullptr) {
        fake->resumed->fetch_add(1, std::memory_order_release);
    }
    return true;
}

void fake_release(void* state) noexcept {
    auto* fake = static_cast<FakeCoroState*>(state);
    if (fake->released != nullptr) {
        fake->released->fetch_add(1, std::memory_order_release);
    }
}

constexpr detail::ReadyEntryHooks kFakeCoroHooks{
    .owner_scheduler = fake_owner_scheduler,
    .resume_owner_only = fake_resume_owner_only,
    .resume = fake_resume,
    .release = fake_release,
};

detail::ReadyEntry make_fake_coro_entry(FakeCoroState* state) {
    state->header.hooks = &kFakeCoroHooks;
    return detail::ReadyEntry(detail::ReadyEntryKind::CCoroutine, state);
}

bool verify_ready_entry_task_ref_round_trip() {
    TaskRef original = make_dummy_task_ref();
    TaskState* const state = original.state();
    detail::ReadyEntry entry(std::move(original));

    if (original.is_valid()) {
        std::cerr << "[T135] TaskRef should be moved into ReadyEntry\n";
        return false;
    }
    if (!entry.is_cpp_task() || entry.state() != state) {
        std::cerr << "[T135] ReadyEntry should carry the original C++ TaskState\n";
        return false;
    }

    TaskRef restored = detail::ready_entry_to_task_ref(entry);
    if (restored.state() != state || entry.is_valid()) {
        std::cerr << "[T135] ReadyEntry should release ownership back to TaskRef\n";
        return false;
    }
    return true;
}

bool verify_c_coroutine_ready_entry_hooks() {
    NullScheduler owner;
    std::atomic<int> resumed{0};
    std::atomic<int> released{0};
    FakeCoroState state{
        .owner = &owner,
        .owner_only = true,
        .resumed = &resumed,
        .released = &released,
    };

    detail::ReadyEntry entry = make_fake_coro_entry(&state);
    if (!entry.is_valid() || entry.kind() != detail::ReadyEntryKind::CCoroutine) {
        std::cerr << "[T135] C coroutine ReadyEntry should be valid\n";
        return false;
    }
    if (detail::ready_entry_scheduler(entry) != &owner) {
        std::cerr << "[T135] C coroutine ReadyEntry should expose owner scheduler\n";
        return false;
    }
    if (!detail::ready_entry_resume_owner_only(entry)) {
        std::cerr << "[T135] C coroutine ReadyEntry should expose owner-only resume\n";
        return false;
    }
    if (!detail::resume_ready_entry(entry) ||
        resumed.load(std::memory_order_acquire) != 1 ||
        released.load(std::memory_order_acquire) != 1 ||
        entry.is_valid()) {
        std::cerr << "[T135] C coroutine ReadyEntry should resume through hook and release ownership\n";
        return false;
    }

    return true;
}

bool verify_c_coroutine_schedule_reject_keeps_entry() {
    std::atomic<int> released{0};
    FakeCoroState state{.released = &released};
    detail::ReadyEntry entry = make_fake_coro_entry(&state);

    if (detail::schedule_ready_entry(entry) || !entry.is_valid() ||
        released.load(std::memory_order_acquire) != 0) {
        std::cerr << "[T135] unsupported C coroutine schedule should reject without release\n";
        return false;
    }

    detail::release_ready_entry(entry);
    if (released.load(std::memory_order_acquire) != 1) {
        std::cerr << "[T135] rejected C coroutine entry should remain owned by caller\n";
        return false;
    }
    return true;
}

bool verify_cpp_schedule_reject_keeps_entry() {
    NullScheduler rejecting_scheduler;
    TaskRef task = make_dummy_task_ref();
    TaskState* const state = task.state();
    detail::set_task_scheduler(task, &rejecting_scheduler);
    detail::ReadyEntry entry(std::move(task));

    if (detail::schedule_ready_entry(entry) || !entry.is_valid() ||
        entry.task_state() != state) {
        std::cerr << "[T135] rejected C++ schedule should keep ReadyEntry owned by caller\n";
        return false;
    }

    TaskRef restored = detail::ready_entry_to_task_ref(entry);
    if (!restored.is_valid() || restored.state() != state ||
        restored.belong_scheduler() != &rejecting_scheduler) {
        std::cerr << "[T135] rejected C++ schedule should remain recoverable as TaskRef\n";
        return false;
    }
    return true;
}

bool verify_scheduler_core_accepts_ready_entry_resume() {
    IOReadyQueue worker;
    SchedulerCore core(worker, 4);
    worker.schedule_local(make_dummy_task_ref());

    size_t ready_entries = 0;
    const size_t ran = core.run_ready_pass([&](detail::ReadyEntry& entry) {
        if (!entry.is_cpp_task()) {
            return;
        }
        TaskRef task = detail::ready_entry_to_task_ref(entry);
        if (task.is_valid()) {
            ++ready_entries;
        }
    });

    if (ran != 1 || ready_entries != 1) {
        std::cerr << "[T135] SchedulerCore should expose ReadyEntry to compatible callbacks\n";
        return false;
    }
    return true;
}

bool verify_owner_only_c_coroutine_entry_is_returned_to_caller() {
    std::atomic<int> released{0};
    FakeCoroState state{
        .owner_only = true,
        .released = &released,
    };
    detail::ReadyEntry entry = make_fake_coro_entry(&state);
    ChaseLevTaskRing ring;

    if (!ring.push_back(entry) || entry.is_valid()) {
        std::cerr << "[T135] failed to queue fake C coroutine entry\n";
        return false;
    }

    detail::ReadyEntry stolen;
    if (ring.steal_front(stolen) || !stolen.is_valid()) {
        std::cerr << "[T135] owner-only C coroutine entry should be returned to caller after CAS\n";
        detail::release_ready_entry(stolen);
        return false;
    }
    if (ring.size() != 0 || released.load(std::memory_order_acquire) != 0) {
        std::cerr << "[T135] ring should not pre-CAS peek or release owner-only C coroutine entry\n";
        return false;
    }

    detail::release_ready_entry(stolen);
    if (released.load(std::memory_order_acquire) != 1) {
        std::cerr << "[T135] returned owner-only C coroutine entry should release exactly once\n";
        return false;
    }
    return true;
}

bool verify_worker_requeues_owner_only_entry_through_inject_queue() {
    std::atomic<int> released{0};
    FakeCoroState state{
        .owner_only = true,
        .released = &released,
    };
    detail::ReadyEntry entry = make_fake_coro_entry(&state);
    IOReadyQueue worker;

    if (!worker.local_ring.push_back(entry)) {
        std::cerr << "[T135] failed to queue worker owner-only probe\n";
        return false;
    }

    detail::ReadyEntry stolen;
    if (worker.steal_front(stolen) || stolen.is_valid()) {
        std::cerr << "[T135] worker owner-only probe must not be exposed to stealer\n";
        detail::release_ready_entry(stolen);
        return false;
    }
    if (!worker.has_pending_injected() || worker.local_ring.size() != 0 ||
        released.load(std::memory_order_acquire) != 0) {
        std::cerr << "[T135] worker should requeue owner-only entry through inject queue\n";
        return false;
    }

    if (worker.drain_injected() != 1 || !worker.pop_next(stolen)) {
        std::cerr << "[T135] owner should recover worker-requeued owner-only entry\n";
        return false;
    }
    detail::release_ready_entry(stolen);
    return released.load(std::memory_order_acquire) == 1;
}

bool verify_pending_ready_entries_release_on_worker_destroy() {
    std::atomic<int> released{0};
    FakeCoroState ring_state{.released = &released};
    FakeCoroState lifo_state{.released = &released};
    FakeCoroState injected_state{.released = &released};
    FakeCoroState buffer_state{.released = &released};

    {
        IOReadyQueue worker(2);
        worker.schedule_local(make_fake_coro_entry(&ring_state));
        worker.schedule_local(make_fake_coro_entry(&lifo_state));
        worker.schedule_injected(make_fake_coro_entry(&injected_state));
        worker.ready_inject_buffer[0] = make_fake_coro_entry(&buffer_state);
    }

    if (released.load(std::memory_order_acquire) != 4) {
        std::cerr << "[T135] pending ReadyEntry cleanup should release LIFO/ring/inject/buffer entries, released="
                  << released.load(std::memory_order_acquire) << "\n";
        return false;
    }
    return true;
}

bool verify_runtime_cpp_compatibility() {
    Runtime runtime = RuntimeBuilder()
        .io_scheduler_count(1)
        .parallel_scheduler_count(0)
        .build();

    std::atomic<int> spawned{0};
    auto void_handle = runtime.spawn_io(mark_task(&spawned));
    if (!void_handle.has_value()) {
        std::cerr << "[T135] spawnIO(Task<void>) failed\n";
        return false;
    }
    auto void_join = void_handle->join();
    if (!void_join.has_value() || spawned.load(std::memory_order_acquire) != 1) {
        std::cerr << "[T135] spawnIO(Task<void>) did not complete\n";
        runtime.stop();
        return false;
    }

    auto value_handle = runtime.spawn_io(value_task(9));
    if (!value_handle.has_value()) {
        std::cerr << "[T135] spawnIO(Task<int>) failed\n";
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
    auto parent = runtime.block_on_io(parent_task(&parent_steps));
    if (!parent.has_value() || *parent != 43 ||
        parent_steps.load(std::memory_order_acquire) != 2) {
        std::cerr << "[T135] co_await Task<T> parent resume failed\n";
        runtime.stop();
        return false;
    }

    runtime.stop();
    return true;
}

bool verify_then_compatibility() {
    Runtime runtime = RuntimeBuilder()
        .io_scheduler_count(1)
        .parallel_scheduler_count(1)
        .build();

    std::atomic<int> sequence{0};
    auto result = runtime.block_on_io(then_step(&sequence, 0, 1).then(then_step(&sequence, 1, 2)));
    runtime.stop();

    if (!result.has_value() || sequence.load(std::memory_order_acquire) != 2) {
        std::cerr << "[T135] then() continuation did not run in order\n";
        return false;
    }
    return true;
}

bool verify_cross_thread_wake_and_coalescing() {
    Runtime runtime = RuntimeBuilder()
        .io_scheduler_count(1)
        .parallel_scheduler_count(0)
        .build();
    auto started = runtime.start();
    if (!started.has_value()) {
        std::cerr << "[T135] failed to start runtime for cross-thread wake test\n";
        return false;
    }

    auto* scheduler = runtime.get_next_io_scheduler();
    if (scheduler == nullptr) {
        std::cerr << "[T135] missing IO scheduler\n";
        return false;
    }

    ManualWakeState state;
    if (!schedule_task(*scheduler, parked_task(&state))) {
        std::cerr << "[T135] failed to schedule parked task\n";
        runtime.stop();
        return false;
    }

    if (!wait_until([&]() { return state.armed.load(std::memory_order_acquire); })) {
        std::cerr << "[T135] parked task did not arm waker\n";
        runtime.stop();
        return false;
    }

    std::thread producer([&]() {
        state.waker.wake_up();
        state.waker.wake_up();
        state.waker.wake_up();
    });
    producer.join();

    const bool resumed_once = wait_until([&]() {
        return state.resumed.load(std::memory_order_acquire) == 1;
    });
    runtime.stop();

    if (!resumed_once || state.resumed.load(std::memory_order_acquire) != 1) {
        std::cerr << "[T135] cross-thread wake should resume exactly once, got "
                  << state.resumed.load(std::memory_order_acquire) << "\n";
        return false;
    }
    if (state.resumed_thread != scheduler->thread_id()) {
        std::cerr << "[T135] cross-thread wake resumed on non-owner scheduler thread\n";
        return false;
    }
    return true;
}

}  // namespace

int main() {
    if (!verify_ready_entry_task_ref_round_trip()) {
        return 1;
    }
    if (!verify_c_coroutine_ready_entry_hooks()) {
        return 1;
    }
    if (!verify_c_coroutine_schedule_reject_keeps_entry()) {
        return 1;
    }
    if (!verify_cpp_schedule_reject_keeps_entry()) {
        return 1;
    }
    if (!verify_scheduler_core_accepts_ready_entry_resume()) {
        return 1;
    }
    if (!verify_owner_only_c_coroutine_entry_is_returned_to_caller()) {
        return 1;
    }
    if (!verify_worker_requeues_owner_only_entry_through_inject_queue()) {
        return 1;
    }
    if (!verify_pending_ready_entries_release_on_worker_destroy()) {
        return 1;
    }
    if (!verify_runtime_cpp_compatibility()) {
        return 1;
    }
    if (!verify_then_compatibility()) {
        return 1;
    }
    if (!verify_cross_thread_wake_and_coalescing()) {
        return 1;
    }

    std::cout << "T135-ReadyEntryCppCompat PASS\n";
    return 0;
}
