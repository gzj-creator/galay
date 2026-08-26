/**
 * @file e12_policy.cc
 * @brief 用最小标准 await_* 三件套演示显式 timeout policy 自定义 awaitable。
 *
 * 这个例子不依赖协议或 socket：awaitable 只负责把跨线程 signal 映射成结果，
 * `TimeoutSupport<Awaitable, Policy>` 负责超时裁决。两个类型参数都在编译期确定。
 */

#include <galay/cpp/galay-kernel/async/async_waiter.h>
#include <galay/cpp/galay-kernel/core/runtime.h>
#include <galay/cpp/galay-kernel/core/task.h>

#include <atomic>
#include <chrono>
#include <expected>
#include <iostream>
#include <thread>

using namespace galay::kernel;
using namespace std::chrono_literals;

namespace {

using Result = std::expected<int, IOError>;

struct SignalAwaitable;

struct SignalTimeoutPolicy {
    static void inject(SignalAwaitable& awaitable) noexcept;

    static bool ownsIoRegistration(SignalAwaitable&) noexcept {
        return true;
    }
};

struct SignalAwaitable
    : TimeoutSupport<SignalAwaitable, SignalTimeoutPolicy> {
    explicit SignalAwaitable(AsyncWaiter<void>& signal) noexcept
        : m_signal(&signal) {}

    bool await_ready() const noexcept {
        return m_signal->isReady();
    }

    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> handle) noexcept {
        auto waiter = m_signal->wait();
        return waiter.await_suspend(handle);
    }

    Result await_resume() noexcept {
        if (m_timed_out) {
            return std::unexpected(IOError(kTimeout, 0));
        }
        return 42;
    }

    void setTimeout() noexcept {
        m_timed_out = true;
        auto waiter = m_signal->wait();
        waiter.markTimeout();
    }

private:
    AsyncWaiter<void>* m_signal = nullptr;
    bool m_timed_out = false;
};

void SignalTimeoutPolicy::inject(SignalAwaitable& awaitable) noexcept {
    awaitable.setTimeout();
}

struct DemoState {
    std::atomic<bool> done{false};
    std::atomic<bool> success{false};
};

Task<void> waitForSignal(AsyncWaiter<void>* signal, DemoState* state) {
    auto result = co_await SignalAwaitable(*signal).timeout(500ms);
    state->success.store(result.has_value() && result.value() == 42,
                         std::memory_order_release);
    state->done.store(true, std::memory_order_release);
}

bool waitUntil(const std::atomic<bool>& flag) {
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (flag.load(std::memory_order_acquire)) {
            return true;
        }
        std::this_thread::sleep_for(1ms);
    }
    return flag.load(std::memory_order_acquire);
}

}  // namespace

int main() {
    Runtime runtime = RuntimeBuilder()
        .ioSchedulerCount(1)
        .parallelSchedulerCount(0)
        .build();
    auto started = runtime.start();
    if (!started.has_value()) {
        std::cerr << "runtime failed to start\n";
        return 1;
    }

    AsyncWaiter<void> signal;
    DemoState state;
    auto task = waitForSignal(&signal, &state);
    auto* scheduler = runtime.getNextIOScheduler();
    const bool submitted = scheduler != nullptr &&
        scheduler->schedule(detail::TaskAccess::detachTask(std::move(task)));

    std::thread producer([&signal]() {
        std::this_thread::sleep_for(10ms);
        signal.notify();
    });

    const bool completed = submitted && waitUntil(state.done);
    runtime.stop();
    producer.join();

    if (!completed || !state.success.load(std::memory_order_acquire)) {
        std::cerr << "custom timeout-policy awaitable failed\n";
        return 1;
    }

    std::cout << "custom timeout-policy awaitable passed\n";
    return 0;
}
