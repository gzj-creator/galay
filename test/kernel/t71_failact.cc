/**
 * @file t71_failact.cc
 * @brief 用途：验证状态机的 `Continue` 与 `Fail` 控制动作都会被 Awaitable 正确执行。
 * 关键覆盖点：`MachineAction::continue_()` 本地推进、`MachineAction::fail(...)` 错误透传。
 * 通过条件：状态机先执行一次 Continue，再把 IOError 透传给 await_resume。
 */

#include <galay/cpp/galay-kernel/core/awaitable.h>
#include <galay/cpp/galay-kernel/core/task.h>
#include <atomic>
#include <chrono>
#include <expected>
#include <iostream>
#include <thread>

#ifdef USE_IOURING
#include <galay/cpp/galay-kernel/core/uring_scheduler.h>
using TestScheduler = galay::kernel::IOUringScheduler;
#elif defined(USE_EPOLL)
#include <galay/cpp/galay-kernel/core/epoll_scheduler.h>
using TestScheduler = galay::kernel::EpollScheduler;
#elif defined(USE_KQUEUE)
#include <galay/cpp/galay-kernel/core/kqueue_scheduler.h>
using TestScheduler = galay::kernel::KqueueScheduler;
#endif

using namespace galay::kernel;
using namespace std::chrono_literals;

namespace {

using FailResult = std::expected<int, IOError>;

struct ContinueThenFailMachine {
    using result_type = FailResult;

    MachineAction<result_type> advance() {
        if (!m_continued) {
            m_continued = true;
            return MachineAction<result_type>::continue_();
        }
        return MachineAction<result_type>::fail(IOError(kParamInvalid, 0));
    }

    void onRead(std::expected<size_t, IOError>) {}
    void onWrite(std::expected<size_t, IOError>) {}

private:
    bool m_continued = false;
};

struct TestState {
    std::atomic<bool> done{false};
    std::atomic<bool> success{false};
};

Task<void> failTask(TestState* state) {
    IOController controller(GHandle::invalid());
    StateMachineAwaitable<ContinueThenFailMachine> awaitable(
        &controller,
        ContinueThenFailMachine{}
    );

    auto result = co_await awaitable;
    state->success.store(
        !result.has_value() && IOError::contains(result.error().code(), kParamInvalid),
        std::memory_order_release
    );
    state->done.store(true, std::memory_order_release);
}

bool waitUntil(const std::atomic<bool>& flag,
               std::chrono::milliseconds timeout = 1000ms,
               std::chrono::milliseconds step = 2ms) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (flag.load(std::memory_order_acquire)) {
            return true;
        }
        std::this_thread::sleep_for(step);
    }
    return flag.load(std::memory_order_acquire);
}

}  // namespace

int main() {
    TestScheduler scheduler;
    scheduler.start();

    TestState state;
    scheduleTask(scheduler, failTask(&state));

    const bool completed = waitUntil(state.done);
    scheduler.stop();

    if (!completed) {
        std::cerr << "[T71] state machine fail action timed out\n";
        return 1;
    }
    if (!state.success.load(std::memory_order_acquire)) {
        std::cerr << "[T71] state machine fail action mismatch\n";
        return 1;
    }

    std::cout << "T71-StateMachineFailAction PASS\n";
    return 0;
}
