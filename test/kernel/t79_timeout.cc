/**
 * @file t79_timeout.cc
 * @brief 用途：验证共享 state-machine awaitable 家族支持 `.timeout(...)` 并返回超时错误。
 * 关键覆盖点：`StateMachineAwaitable::timeout(...)`、
 * `AwaitableBuilder::fromStateMachine(...).build().timeout(...)`。
 * 通过条件：目标成功编译，direct/builder 两条路径都在超时后返回 `kTimeout`。
 */

#include "galay-kernel/core/awaitable.h"
#include "galay-kernel/core/task.h"
#include <atomic>
#include <chrono>
#include <concepts>
#include <expected>
#include <fcntl.h>
#include <iostream>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

#ifdef USE_IOURING
#include "galay-kernel/core/uring_scheduler.h"
using TestScheduler = galay::kernel::IOUringScheduler;
#elif defined(USE_EPOLL)
#include "galay-kernel/core/epoll_scheduler.h"
using TestScheduler = galay::kernel::EpollScheduler;
#elif defined(USE_KQUEUE)
#include "galay-kernel/core/kqueue_scheduler.h"
using TestScheduler = galay::kernel::KqueueScheduler;
#endif

using namespace galay::kernel;
using namespace std::chrono_literals;

namespace {

using TimeoutResult = std::expected<size_t, IOError>;

struct TimeoutMachine {
    using result_type = TimeoutResult;

    MachineAction<result_type> advance() {
        return MachineAction<result_type>::waitRead(&m_byte, 1);
    }

    void onRead(std::expected<size_t, IOError> result) {
        m_result = std::move(result);
    }

    void onWrite(std::expected<size_t, IOError>) {}

    char m_byte = 0;
    std::optional<TimeoutResult> m_result;
};

template <typename AwaitableT>
concept HasTimeout = requires(AwaitableT awaitable) {
    { awaitable.timeout(1ms) };
};

static_assert(HasTimeout<StateMachineAwaitable<TimeoutMachine>>);
static_assert(requires(IOController* controller) {
    { AwaitableBuilder<TimeoutResult>::fromStateMachine(controller, TimeoutMachine{}).build().timeout(1ms) };
});

struct TestState {
    std::atomic<bool> direct_done{false};
    std::atomic<bool> builder_done{false};
    std::atomic<int> direct_error{0};
    std::atomic<int> builder_error{0};
};

Task<void> directTimeoutTask(TestState* state, int fd) {
    IOController controller(GHandle{.fd = fd});
    auto result = co_await StateMachineAwaitable<TimeoutMachine>(&controller, TimeoutMachine{}).timeout(50ms);
    state->direct_error.store(result ? 0 : static_cast<int>(result.error().code()), std::memory_order_release);
    state->direct_done.store(true, std::memory_order_release);
}

Task<void> builderTimeoutTask(TestState* state, int fd) {
    IOController controller(GHandle{.fd = fd});
    auto awaitable = AwaitableBuilder<TimeoutResult>::fromStateMachine(&controller, TimeoutMachine{}).build();
    auto result = co_await awaitable.timeout(50ms);
    state->builder_error.store(result ? 0 : static_cast<int>(result.error().code()), std::memory_order_release);
    state->builder_done.store(true, std::memory_order_release);
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

bool setNonBlocking(int fd) {
    const int flags = fcntl(fd, F_GETFL, 0);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

}  // namespace

int main() {
    int direct_fds[2] = {-1, -1};
    int builder_fds[2] = {-1, -1};
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, direct_fds) != 0) {
        std::perror("[T79] direct socketpair");
        return 1;
    }
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, builder_fds) != 0) {
        std::perror("[T79] builder socketpair");
        close(direct_fds[0]);
        close(direct_fds[1]);
        return 1;
    }
    if (!setNonBlocking(direct_fds[0]) || !setNonBlocking(direct_fds[1]) ||
        !setNonBlocking(builder_fds[0]) || !setNonBlocking(builder_fds[1])) {
        std::cerr << "[T79] failed to set non-blocking mode\n";
        close(direct_fds[0]);
        close(direct_fds[1]);
        close(builder_fds[0]);
        close(builder_fds[1]);
        return 1;
    }

    TestScheduler scheduler;
    scheduler.start();

    TestState state;
    scheduleTask(scheduler, directTimeoutTask(&state, direct_fds[0]));
    scheduleTask(scheduler, builderTimeoutTask(&state, builder_fds[0]));

    const bool direct_completed = waitUntil(state.direct_done);
    const bool builder_completed = waitUntil(state.builder_done);

    scheduler.stop();
    close(direct_fds[0]);
    close(direct_fds[1]);
    close(builder_fds[0]);
    close(builder_fds[1]);

    if (!direct_completed) {
        std::cerr << "[T79] direct state-machine timeout task did not complete\n";
        return 1;
    }
    if (!builder_completed) {
        std::cerr << "[T79] builder state-machine timeout task did not complete\n";
        return 1;
    }
    if (state.direct_error.load(std::memory_order_acquire) != static_cast<int>(kTimeout)) {
        std::cerr << "[T79] direct state-machine timeout error mismatch: "
                  << state.direct_error.load(std::memory_order_acquire) << "\n";
        return 1;
    }
    if (state.builder_error.load(std::memory_order_acquire) != static_cast<int>(kTimeout)) {
        std::cerr << "[T79] builder state-machine timeout error mismatch: "
                  << state.builder_error.load(std::memory_order_acquire) << "\n";
        return 1;
    }

    std::cout << "T79-StateMachineTimeout PASS\n";
    return 0;
}
