#include "galay-kernel/core/awaitable.h"
#include "galay-kernel/core/task.h"
#include <atomic>
#include <chrono>
#include <expected>
#include <fcntl.h>
#include <iostream>
#include <optional>
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

enum class DomainErrorCode : int {
    kNone = 0,
    kTimeout = 1,
    kOther = 2,
};

struct DomainError {
    DomainErrorCode code = DomainErrorCode::kNone;
};

using DomainResult = std::expected<size_t, DomainError>;

struct DomainMachine {
    using result_type = DomainResult;

    MachineAction<result_type> advance() {
        if (m_result.has_value()) {
            return MachineAction<result_type>::complete(std::move(*m_result));
        }
        return MachineAction<result_type>::waitRead(&m_byte, 1);
    }

    void onRead(std::expected<size_t, IOError> result) {
        if (!result) {
            if (IOError::contains(result.error().code(), kTimeout)) {
                m_result = std::unexpected(DomainError{DomainErrorCode::kTimeout});
            } else {
                m_result = std::unexpected(DomainError{DomainErrorCode::kOther});
            }
            return;
        }
        m_result = result.value();
    }

    void onWrite(std::expected<size_t, IOError>) {}

    char m_byte = 0;
    std::optional<DomainResult> m_result;
};

struct TestState {
    std::atomic<bool> direct_done{false};
    std::atomic<bool> builder_done{false};
    std::atomic<int> direct_error{0};
    std::atomic<int> builder_error{0};
};

Task<void> directTimeoutTask(TestState* state, int fd) {
    IOController controller(GHandle{.fd = fd});
    auto result = co_await StateMachineAwaitable<DomainMachine>(&controller, DomainMachine{}).timeout(50ms);
    state->direct_error.store(result ? 0 : static_cast<int>(result.error().code), std::memory_order_release);
    state->direct_done.store(true, std::memory_order_release);
}

Task<void> builderTimeoutTask(TestState* state, int fd) {
    IOController controller(GHandle{.fd = fd});
    auto awaitable = AwaitableBuilder<DomainResult>::fromStateMachine(&controller, DomainMachine{}).build();
    auto result = co_await awaitable.timeout(50ms);
    state->builder_error.store(result ? 0 : static_cast<int>(result.error().code), std::memory_order_release);
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
        std::perror("[T86] direct socketpair");
        return 1;
    }
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, builder_fds) != 0) {
        std::perror("[T86] builder socketpair");
        close(direct_fds[0]);
        close(direct_fds[1]);
        return 1;
    }
    if (!setNonBlocking(direct_fds[0]) || !setNonBlocking(direct_fds[1]) ||
        !setNonBlocking(builder_fds[0]) || !setNonBlocking(builder_fds[1])) {
        std::cerr << "[T86] failed to set non-blocking mode\n";
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
        std::cerr << "[T86] direct timeout task did not complete\n";
        return 1;
    }
    if (!builder_completed) {
        std::cerr << "[T86] builder timeout task did not complete\n";
        return 1;
    }
    if (state.direct_error.load(std::memory_order_acquire) != static_cast<int>(DomainErrorCode::kTimeout)) {
        std::cerr << "[T86] direct timeout error mismatch: "
                  << state.direct_error.load(std::memory_order_acquire) << "\n";
        return 1;
    }
    if (state.builder_error.load(std::memory_order_acquire) != static_cast<int>(DomainErrorCode::kTimeout)) {
        std::cerr << "[T86] builder timeout error mismatch: "
                  << state.builder_error.load(std::memory_order_acquire) << "\n";
        return 1;
    }

    std::cout << "T86-StateMachineTimeoutErrorBridge PASS\n";
    return 0;
}
