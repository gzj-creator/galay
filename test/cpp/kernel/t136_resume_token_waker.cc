/**
 * @file t136_resume_token_waker.cc
 * @brief 验证 Waker 通过语言中立 ResumeToken 保持 C++ 行为并支持 C 协程 hook。
 */

#include <galay/cpp/galay-kernel/core/runtime.h>
#include <galay/cpp/galay-kernel/core/scheduler.hpp>
#include <galay/cpp/galay-kernel/core/waker.h>

#include <atomic>
#include <chrono>
#include <coroutine>
#include <iostream>
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

struct FakeResumeTokenState {
    detail::ResumeTokenHeader header;
    Scheduler* owner = nullptr;
    std::atomic<int> retain_count{0};
    std::atomic<int> release_count{0};
    std::atomic<int> resume_requests{0};
};

Scheduler* fakeTokenOwner(void* state) noexcept {
    return static_cast<FakeResumeTokenState*>(state)->owner;
}

bool fakeTokenRequestResume(void* state) noexcept {
    static_cast<FakeResumeTokenState*>(state)->resume_requests.fetch_add(1, std::memory_order_release);
    return true;
}

void fakeTokenRetain(void* state) noexcept {
    static_cast<FakeResumeTokenState*>(state)->retain_count.fetch_add(1, std::memory_order_release);
}

void fakeTokenRelease(void* state) noexcept {
    static_cast<FakeResumeTokenState*>(state)->release_count.fetch_add(1, std::memory_order_release);
}

constexpr detail::ResumeTokenHooks kFakeResumeTokenHooks{
    .owner_scheduler = fakeTokenOwner,
    .request_resume = fakeTokenRequestResume,
    .retain = fakeTokenRetain,
    .release = fakeTokenRelease,
};

detail::ResumeToken makeFakeResumeToken(FakeResumeTokenState* state) {
    state->header.hooks = &kFakeResumeTokenHooks;
    return detail::ResumeToken::fromCCoroutine(state);
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

bool verifyFakeResumeTokenHooks() {
    NullScheduler owner;
    FakeResumeTokenState state{.owner = &owner};

    {
        Waker waker(makeFakeResumeToken(&state));
        if (waker.getScheduler() != &owner) {
            std::cerr << "[T136] fake C resume token should expose owner scheduler\n";
            return false;
        }

        Waker copy = waker;
        waker.wakeUp();
        copy.wakeUp();
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

bool verifyMisalignedResumeTokenIsIgnored() {
    alignas(8) unsigned char storage[sizeof(FakeResumeTokenState) + 4]{};
    auto* state = new (storage) FakeResumeTokenState{};
    state->owner = nullptr;
    state->header.hooks = &kFakeResumeTokenHooks;
    void* misaligned = storage + 1;

    {
        Waker waker(detail::ResumeToken::fromCCoroutine(misaligned));
        if (waker.getScheduler() != nullptr) {
            std::cerr << "[T136] misaligned C resume token should be rejected\n";
            state->~FakeResumeTokenState();
            return false;
        }
        waker.wakeUp();
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

bool verifyCppWakerStillCoalesces() {
    Runtime runtime = RuntimeBuilder()
        .ioSchedulerCount(1)
        .computeSchedulerCount(0)
        .build();
    auto started = runtime.start();
    if (!started.has_value()) {
        std::cerr << "[T136] failed to start runtime for C++ Waker test\n";
        return false;
    }

    auto* scheduler = runtime.getNextIOScheduler();
    if (scheduler == nullptr) {
        std::cerr << "[T136] missing IO scheduler\n";
        runtime.stop();
        return false;
    }

    ManualWakeState state;
    if (!scheduleTask(*scheduler, parkedTask(&state))) {
        std::cerr << "[T136] failed to schedule parked task\n";
        runtime.stop();
        return false;
    }

    if (!waitUntil([&]() { return state.armed.load(std::memory_order_acquire); })) {
        std::cerr << "[T136] parked task did not arm waker\n";
        runtime.stop();
        return false;
    }

    std::thread producer([&]() {
        Waker copy = state.waker;
        state.waker.wakeUp();
        copy.wakeUp();
        state.waker.wakeUp();
    });
    producer.join();

    const bool resumed_once = waitUntil([&]() {
        return state.resumed.load(std::memory_order_acquire) == 1;
    });
    runtime.stop();

    if (!resumed_once || state.resumed.load(std::memory_order_acquire) != 1) {
        std::cerr << "[T136] C++ Waker should coalesce duplicate wake requests\n";
        return false;
    }
    if (state.resumed_thread != scheduler->threadId()) {
        std::cerr << "[T136] C++ Waker should resume on owner scheduler thread\n";
        return false;
    }
    return true;
}

bool verifyInvalidWakerIsIgnored() {
    Waker waker;
    if (waker.getScheduler() != nullptr) {
        std::cerr << "[T136] empty Waker should not expose a scheduler\n";
        return false;
    }
    waker.wakeUp();
    return true;
}

}  // namespace

int main() {
    if (!verifyFakeResumeTokenHooks()) {
        return 1;
    }
    if (!verifyMisalignedResumeTokenIsIgnored()) {
        return 1;
    }
    if (!verifyCppWakerStillCoalesces()) {
        return 1;
    }
    if (!verifyInvalidWakerIsIgnored()) {
        return 1;
    }

    std::cout << "T136-ResumeTokenWaker PASS\n";
    return 0;
}
