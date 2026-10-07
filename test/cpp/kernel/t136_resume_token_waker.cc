/**
 * @file t136_resume_token_waker.cc
 * @brief 验证 Waker 通过语言中立 ResumeToken 保持 C++ 行为并支持 C 协程 hook。
 */

#include <galay/cpp/galay-kernel/core/runtime.h>
#include "test/cpp/common/scheduler_test_adapter.h"
#include <galay/cpp/galay-kernel/core/waker.h>

#include <atomic>
#include <chrono>
#include <coroutine>
#include <iostream>
#include <thread>

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

class ResumeOnlyScheduler final : public detail::SchedulerTestAdapter<ResumeOnlyScheduler> {
public:
    ~ResumeOnlyScheduler() { stop(); }

    std::expected<void, IOError> start() {
        bool expected = false;
        if (!m_running.compare_exchange_strong(
                expected, true, std::memory_order_acq_rel)) {
            return {};
        }
        m_thread = std::thread([this]() {
            m_threadId = std::this_thread::get_id();
            m_ready.store(true, std::memory_order_release);
            while (m_running.load(std::memory_order_acquire)) {
                drain();
                std::this_thread::yield();
            }
            drain();
        });
        while (!m_ready.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        return {};
    }

    void stop() {
        if (!m_running.exchange(false, std::memory_order_acq_rel)) {
            return;
        }
        if (m_thread.joinable()) {
            m_thread.join();
        }
    }

    bool schedule(TaskRef) noexcept {
        m_regularScheduleCalls.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    bool schedule_resume(TaskRef task) noexcept {
        if (!bind_task(task)) {
            return false;
        }
        m_resumeScheduleCalls.fetch_add(1, std::memory_order_relaxed);
        return m_resumeQueue.push(std::move(task));
    }

    bool schedule_deferred(TaskRef) noexcept { return false; }

    bool schedule_immediately(TaskRef task) noexcept {
        if (!bind_task(task)) {
            return false;
        }
        resume(task);
        return true;
    }

    bool add_timer(Timer::ptr) { return false; }
    SchedulerType type() { return kParallelScheduler; }

    int regular_schedule_calls() const noexcept {
        return m_regularScheduleCalls.load(std::memory_order_acquire);
    }

    int resume_schedule_calls() const noexcept {
        return m_resumeScheduleCalls.load(std::memory_order_acquire);
    }

private:
    void drain() {
        TaskState* ready = detail::TaskResumeQueue::reverse(
            m_resumeQueue.take_all());
        while (ready != nullptr) {
            TaskRef task = detail::TaskResumeQueue::pop_front(ready);
            resume(task);
        }
    }

    std::thread m_thread;
    detail::TaskResumeQueue m_resumeQueue;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_ready{false};
    std::atomic<int> m_regularScheduleCalls{0};
    std::atomic<int> m_resumeScheduleCalls{0};
};

struct FakeResumeTokenState {
    detail::ResumeTokenHeader header;
    Scheduler* owner = nullptr;
    std::atomic<int> retain_count{0};
    std::atomic<int> release_count{0};
    std::atomic<int> resume_requests{0};
};

Scheduler* fake_token_owner(void* state) noexcept {
    return static_cast<FakeResumeTokenState*>(state)->owner;
}

bool fake_token_request_resume(void* state) noexcept {
    static_cast<FakeResumeTokenState*>(state)->resume_requests.fetch_add(1, std::memory_order_release);
    return true;
}

void fake_token_retain(void* state) noexcept {
    static_cast<FakeResumeTokenState*>(state)->retain_count.fetch_add(1, std::memory_order_release);
}

void fake_token_release(void* state) noexcept {
    static_cast<FakeResumeTokenState*>(state)->release_count.fetch_add(1, std::memory_order_release);
}

constexpr detail::ResumeTokenHooks kFakeResumeTokenHooks{
    .owner_scheduler = fake_token_owner,
    .request_resume = fake_token_request_resume,
    .retain = fake_token_retain,
    .release = fake_token_release,
};

detail::ResumeToken make_fake_resume_token(FakeResumeTokenState* state) {
    state->header.hooks = &kFakeResumeTokenHooks;
    return detail::ResumeToken::from_c_coroutine(state);
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

Task<void> completed_child() {
    co_return;
}

Task<void> parent_awaiting_completed_child(ManualWakeState* state) {
    auto child = co_await completed_child();
    if (!child.has_value()) {
        co_return;
    }
    state->resumed_thread = std::this_thread::get_id();
    state->resumed.fetch_add(1, std::memory_order_release);
    co_return;
}

bool verify_fake_resume_token_hooks() {
    NullScheduler owner;
    FakeResumeTokenState state{.owner = &owner};

    {
        Waker waker(make_fake_resume_token(&state));
        if (waker.get_scheduler() != &owner) {
            std::cerr << "[T136] fake C resume token should expose owner scheduler\n";
            return false;
        }

        Waker copy = waker;
        waker.wake_up();
        copy.wake_up();
    }

    if (state.retain_count.load(std::memory_order_acquire) != 2 ||
        state.release_count.load(std::memory_order_acquire) != 2 ||
        state.resume_requests.load(std::memory_order_acquire) != 2) {
        std::cerr << "[T136] fake C resume token retain/release/request mismatch"
                  << ", retain=" << state.retain_count.load(std::memory_order_acquire)
                  << ", release=" << state.release_count.load(std::memory_order_acquire)
                  << ", request=" << state.resume_requests.load(std::memory_order_acquire)
                  << "\n";
        return false;
    }

    return true;
}

bool verify_misaligned_resume_token_is_ignored() {
    alignas(8) unsigned char storage[sizeof(FakeResumeTokenState) + 4]{};
    auto* state = new (storage) FakeResumeTokenState{};
    state->owner = nullptr;
    state->header.hooks = &kFakeResumeTokenHooks;
    void* misaligned = storage + 1;

    {
        Waker waker(detail::ResumeToken::from_c_coroutine(misaligned));
        if (waker.get_scheduler() != nullptr) {
            std::cerr << "[T136] misaligned C resume token should be rejected\n";
            state->~FakeResumeTokenState();
            return false;
        }
        waker.wake_up();
    }

    if (state->retain_count.load(std::memory_order_acquire) != 0 ||
        state->release_count.load(std::memory_order_acquire) != 0 ||
        state->resume_requests.load(std::memory_order_acquire) != 0) {
        std::cerr << "[T136] rejected misaligned C resume token should not touch hooks\n";
        state->~FakeResumeTokenState();
        return false;
    }

    state->~FakeResumeTokenState();
    return true;
}

bool verify_cpp_waker_still_coalesces() {
    Runtime runtime = RuntimeBuilder()
        .io_scheduler_count(1)
        .parallel_scheduler_count(0)
        .build();
    auto started = runtime.start();
    if (!started.has_value()) {
        std::cerr << "[T136] failed to start runtime for C++ Waker test\n";
        return false;
    }

    auto* scheduler = runtime.get_next_io_scheduler();
    if (scheduler == nullptr) {
        std::cerr << "[T136] missing IO scheduler\n";
        runtime.stop();
        return false;
    }

    ManualWakeState state;
    if (!schedule_task(*scheduler, parked_task(&state))) {
        std::cerr << "[T136] failed to schedule parked task\n";
        runtime.stop();
        return false;
    }

    if (!wait_until([&]() { return state.armed.load(std::memory_order_acquire); })) {
        std::cerr << "[T136] parked task did not arm waker\n";
        runtime.stop();
        return false;
    }

    std::thread producer([&]() {
        Waker copy = state.waker;
        state.waker.wake_up();
        copy.wake_up();
        state.waker.wake_up();
    });
    producer.join();

    const bool resumed_once = wait_until([&]() {
        return state.resumed.load(std::memory_order_acquire) == 1;
    });
    runtime.stop();

    if (!resumed_once || state.resumed.load(std::memory_order_acquire) != 1) {
        std::cerr << "[T136] C++ Waker should coalesce duplicate wake requests\n";
        return false;
    }
    if (state.resumed_thread != scheduler->thread_id()) {
        std::cerr << "[T136] C++ Waker should resume on owner scheduler thread\n";
        return false;
    }
    return true;
}

bool verify_cpp_waker_uses_resume_admission() {
    ResumeOnlyScheduler scheduler;
    auto started = scheduler.start();
    if (!started.has_value()) {
        std::cerr << "[T136] failed to start resume-only scheduler\n";
        return false;
    }

    ManualWakeState state;
    Task<void> task = parked_task(&state);
    TaskRef scheduled = detail::TaskAccess::detach_task(std::move(task));
    if (!scheduler.schedule_immediately(std::move(scheduled)) ||
        !state.armed.load(std::memory_order_acquire)) {
        std::cerr << "[T136] failed to park task on resume-only scheduler\n";
        scheduler.stop();
        return false;
    }

    state.waker.wake_up();
    const bool resumed = wait_until([&]() {
        return state.resumed.load(std::memory_order_acquire) == 1;
    });
    scheduler.stop();

    if (!resumed || state.resumed_thread != scheduler.thread_id() ||
        scheduler.regular_schedule_calls() != 0 ||
        scheduler.resume_schedule_calls() != 1) {
        std::cerr << "[T136] C++ Waker did not use owner-only resume admission\n";
        return false;
    }
    return true;
}

bool verify_continuation_uses_resume_admission() {
    ResumeOnlyScheduler scheduler;
    auto started = scheduler.start();
    if (!started.has_value()) {
        std::cerr << "[T136] failed to start continuation scheduler\n";
        return false;
    }

    ManualWakeState state;
    Task<void> task = parent_awaiting_completed_child(&state);
    TaskRef scheduled = detail::TaskAccess::detach_task(std::move(task));
    if (!scheduler.schedule_immediately(std::move(scheduled))) {
        std::cerr << "[T136] failed to start parent continuation task\n";
        scheduler.stop();
        return false;
    }

    const bool resumed = wait_until([&]() {
        return state.resumed.load(std::memory_order_acquire) == 1;
    });
    scheduler.stop();
    if (!resumed || state.resumed_thread != scheduler.thread_id() ||
        scheduler.regular_schedule_calls() != 0 ||
        scheduler.resume_schedule_calls() != 1) {
        std::cerr << "[T136] task continuation did not use resume admission\n";
        return false;
    }
    return true;
}

bool verify_invalid_waker_is_ignored() {
    Waker waker;
    if (waker.get_scheduler() != nullptr) {
        std::cerr << "[T136] empty Waker should not expose a scheduler\n";
        return false;
    }
    waker.wake_up();
    return true;
}

}  // namespace

int main() {
    if (!verify_fake_resume_token_hooks()) {
        return 1;
    }
    if (!verify_misaligned_resume_token_is_ignored()) {
        return 1;
    }
    if (!verify_cpp_waker_still_coalesces()) {
        return 1;
    }
    if (!verify_cpp_waker_uses_resume_admission()) {
        return 1;
    }
    if (!verify_continuation_uses_resume_admission()) {
        return 1;
    }
    if (!verify_invalid_waker_is_ignored()) {
        return 1;
    }

    std::cout << "T136-ResumeTokenWaker PASS\n";
    return 0;
}
