/**
 * @file t75_qreject.cc
 * @brief 用途：验证 machine-backed AwaitableBuilder 会显式拒绝 handler 中的 ops.queue(...) 误用。
 * 关键覆盖点：builder handler 若试图向旧 sequence 队列塞步骤，不再静默忽略，而是返回契约错误。
 * 通过条件：awaitable 以 kParamInvalid 失败结束。
 */

#include <galay/cpp/galay-kernel/core/awaitable.h>
#include <galay/cpp/galay-kernel/core/task.h>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <expected>
#include <iostream>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

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

using QueueResult = std::expected<size_t, IOError>;

struct QueueFlow {
    void onRecv(SequenceOps<QueueResult, 4>& ops, RecvIOContext&);
    void onFinish(SequenceOps<QueueResult, 4>& ops);
    void onExtra(SequenceOps<QueueResult, 4>& ops);

    using ExtraStep = LocalSequenceStep<QueueResult, 4, QueueFlow, &QueueFlow::onExtra>;

    std::array<char, 1> scratch{};
    ExtraStep extra{this};
    bool queued = false;
    bool extra_called = false;
    bool finish_called = false;
};

inline void QueueFlow::onRecv(SequenceOps<QueueResult, 4>& ops, RecvIOContext&) {
    ops.queue(extra);
    queued = true;
}

inline void QueueFlow::onFinish(SequenceOps<QueueResult, 4>& ops) {
    finish_called = true;
    ops.complete(1u);
}

inline void QueueFlow::onExtra(SequenceOps<QueueResult, 4>& ops) {
    extra_called = true;
    ops.complete(2u);
}

struct TestState {
    std::atomic<bool> done{false};
    std::atomic<bool> success{false};
};

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

Task<void> queueRejectedTask(TestState* state, int fd) {
    IOController controller(GHandle{.fd = fd});
    QueueFlow flow;
    auto awaitable = AwaitableBuilder<QueueResult, 4, QueueFlow>(&controller, flow)
        .recv<&QueueFlow::onRecv>(flow.scratch.data(), flow.scratch.size())
        .finish<&QueueFlow::onFinish>()
        .build();

    auto result = co_await awaitable;
    state->success.store(
        !result.has_value() &&
            IOError::contains(result.error().code(), kParamInvalid) &&
            flow.queued &&
            !flow.extra_called &&
            !flow.finish_called,
        std::memory_order_release
    );
    state->done.store(true, std::memory_order_release);
}

}  // namespace

int main() {
    int fds[2] = {-1, -1};
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
        std::cerr << "[T75] socketpair failed: " << std::strerror(errno) << "\n";
        return 1;
    }

    TestScheduler scheduler;
    scheduler.start();

    TestState state;
    scheduleTask(scheduler, queueRejectedTask(&state, fds[0]));
    constexpr char payload[] = "x";
    ::send(fds[1], payload, sizeof(payload) - 1, 0);

    const bool completed = waitUntil(state.done);
    scheduler.stop();
    close(fds[0]);
    close(fds[1]);

    if (!completed) {
        std::cerr << "[T75] builder queue rejection timed out\n";
        return 1;
    }
    if (!state.success.load(std::memory_order_acquire)) {
        std::cerr << "[T75] builder queue rejection mismatch\n";
        return 1;
    }

    std::cout << "T75-AwaitableBuilderQueueRejected PASS\n";
    return 0;
}
