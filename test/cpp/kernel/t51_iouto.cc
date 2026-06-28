/**
 * @file t51_iouto.cc
 * @brief 用途：验证 io_uring 下 accept timeout 后 close 的生命周期边界。
 * 关键覆盖点：timeout 不遗留可唤醒的 awaitable，随后 close 能安全失效 fd 与 SQE 状态。
 * 通过条件：accept 返回 kTimeout，close 返回成功，测试不依赖外部 benchmark 二进制。
 */

#include <iostream>

#ifdef USE_IOURING

#include <galay/cpp/galay-kernel/async/tcp_socket.h>
#include <galay/cpp/galay-kernel/core/task.h>
#include <galay/cpp/galay-kernel/core/uring_scheduler.h>

#include <atomic>
#include <chrono>
#include <thread>

using namespace galay::async;
using namespace galay::kernel;
using namespace std::chrono_literals;

namespace {

struct TestState {
    std::atomic<bool> done{false};
    std::atomic<bool> accept_timeout{false};
    std::atomic<bool> close_ok{false};
    std::atomic<int> setup_error{0};
};

Task<void> acceptTimeoutThenClose(TestState* state) {
    auto listener_result = TcpSocket::create(IPType::IPV4);
    if (!listener_result) {
        state->setup_error.store(1, std::memory_order_release);
        state->done.store(true, std::memory_order_release);
        co_return;
    }

    TcpSocket listener = std::move(*listener_result);
    auto reuse = listener.option().handleReuseAddr();
    auto nonblock = listener.option().handleNonBlock();
    auto bind = listener.bind(Host(IPType::IPV4, "127.0.0.1", 0));
    auto listen = listener.listen(16);
    if (!reuse || !nonblock || !bind || !listen) {
        state->setup_error.store(2, std::memory_order_release);
        state->done.store(true, std::memory_order_release);
        co_return;
    }

    Host peer;
    auto accepted = co_await listener.accept(&peer).timeout(5ms);
    state->accept_timeout.store(
        !accepted && IOError::contains(accepted.error().code(), kTimeout),
        std::memory_order_release);

    auto closed = co_await listener.close();
    state->close_ok.store(closed.has_value(), std::memory_order_release);
    state->done.store(true, std::memory_order_release);
}

bool waitDone(const TestState& state) {
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (state.done.load(std::memory_order_acquire)) {
            return true;
        }
        std::this_thread::sleep_for(2ms);
    }
    return state.done.load(std::memory_order_acquire);
}

}  // namespace

int main() {
    IOUringScheduler scheduler;
    auto started = scheduler.start();
    if (!started) {
        std::cerr << "[T51] scheduler start failed: " << started.error().message() << "\n";
        return 1;
    }

    TestState state;
    if (!scheduleTask(scheduler, acceptTimeoutThenClose(&state))) {
        scheduler.stop();
        std::cerr << "[T51] schedule task failed\n";
        return 1;
    }

    const bool completed = waitDone(state);
    scheduler.stop();

    if (!completed) {
        std::cerr << "[T51] task did not complete\n";
        return 1;
    }
    if (state.setup_error.load(std::memory_order_acquire) != 0) {
        std::cerr << "[T51] setup failed code="
                  << state.setup_error.load(std::memory_order_acquire) << "\n";
        return 1;
    }
    if (!state.accept_timeout.load(std::memory_order_acquire)) {
        std::cerr << "[T51] accept did not report kTimeout\n";
        return 1;
    }
    if (!state.close_ok.load(std::memory_order_acquire)) {
        std::cerr << "[T51] close failed after timeout\n";
        return 1;
    }

    std::cout << "T51-IOUringTimeoutCloseLifetime PASS\n";
    return 0;
}

#else

int main() {
    std::cout << "T51-IOUringTimeoutCloseLifetime SKIP\n";
    return 0;
}

#endif
