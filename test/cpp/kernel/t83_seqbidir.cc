/**
 * @file t83_seqbidir.cc
 * @brief 用途：定义目标行为——双向 StateMachineAwaitable 注册后，应独占同一 controller。
 * 关键覆盖点：双向 owner 与单向 owner 的冲突策略。
 * 通过条件：双向 owner 可继续完成；后续单向 sequence 立即返回 `kNotReady`。
 */

#include <galay/cpp/galay-kernel/core/awaitable.h>
#include <galay/cpp/galay-kernel/core/task.h>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <expected>
#include <fcntl.h>
#include <iostream>
#include <optional>
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

using SequenceResult = std::expected<size_t, IOError>;

struct BidirectionalMachine {
    using result_type = SequenceResult;

    explicit BidirectionalMachine(std::atomic<bool>* await_bound)
        : m_await_bound(await_bound) {}

    void on_await_context(const AwaitContext&) {
        if (m_await_bound != nullptr) {
            m_await_bound->store(true, std::memory_order_release);
        }
    }

    MachineAction<result_type> advance() {
        if (m_result.has_value()) {
            return MachineAction<result_type>::complete(std::move(*m_result));
        }
        switch (m_phase) {
        case Phase::kWaitRead:
            return MachineAction<result_type>::wait_read(&m_in, 1);
        case Phase::kWaitWrite:
            return MachineAction<result_type>::wait_write(&m_out, 1);
        case Phase::kDone:
            return MachineAction<result_type>::fail(IOError(kParamInvalid, 0));
        }
        return MachineAction<result_type>::fail(IOError(kParamInvalid, 0));
    }

    void on_read(std::expected<size_t, IOError> result) {
        if (!result) {
            m_result = std::unexpected(result.error());
            m_phase = Phase::kDone;
            return;
        }
        if (m_phase == Phase::kWaitRead && result.value() == 1) {
            m_phase = Phase::kWaitWrite;
            return;
        }
        m_result = std::unexpected(IOError(kReadFailed, 0));
        m_phase = Phase::kDone;
    }

    void on_write(std::expected<size_t, IOError> result) {
        if (!result) {
            m_result = std::unexpected(result.error());
            m_phase = Phase::kDone;
            return;
        }
        if (m_phase == Phase::kWaitWrite && result.value() == 1) {
            m_result = result.value();
            m_phase = Phase::kDone;
            return;
        }
        m_result = std::unexpected(IOError(kSendFailed, 0));
        m_phase = Phase::kDone;
    }

private:
    enum class Phase {
        kWaitRead,
        kWaitWrite,
        kDone,
    };

    Phase m_phase = Phase::kWaitRead;
    std::atomic<bool>* m_await_bound = nullptr;
    char m_in = 0;
    const char m_out = 'o';
    std::optional<result_type> m_result;
};

struct ReadOnlyFlow {
    void on_recv(SequenceOps<SequenceResult, 4>& ops, RecvIOContext& ctx) {
        ops.complete(std::move(ctx.m_result));
    }

    char m_byte = 0;
};

struct SharedState {
    explicit SharedState(int fd)
        : controller(GHandle{.fd = fd}) {}

    IOController controller;
    std::atomic<bool> bidirectional_await_bound{false};
    std::atomic<bool> bidirectional_done{false};
    std::atomic<int> bidirectional_value{0};
    std::atomic<int> bidirectional_error{0};

    std::atomic<bool> second_done{false};
    std::atomic<int> second_error{0};
};

Task<void> bidirectional_task(SharedState* state) {
    auto awaitable =
        StateMachineAwaitable<BidirectionalMachine>(&state->controller, BidirectionalMachine{&state->bidirectional_await_bound});
    auto result = co_await awaitable;
    state->bidirectional_value.store(result ? static_cast<int>(result.value()) : -1, std::memory_order_release);
    state->bidirectional_error.store(result ? 0 : static_cast<int>(result.error().code()), std::memory_order_release);
    state->bidirectional_done.store(true, std::memory_order_release);
}

Task<void> second_single_direction_task(SharedState* state) {
    ReadOnlyFlow flow;
    auto awaitable = AwaitableBuilder<SequenceResult, 4, ReadOnlyFlow>(&state->controller, flow)
        .recv<&ReadOnlyFlow::on_recv>(&flow.m_byte, 1)
        .build();
    auto result = co_await awaitable;
    state->second_error.store(result ? 0 : static_cast<int>(result.error().code()), std::memory_order_release);
    state->second_done.store(true, std::memory_order_release);
}

bool wait_until(auto&& predicate,
               std::chrono::milliseconds timeout = 1000ms,
               std::chrono::milliseconds step = 2ms) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(step);
    }
    return predicate();
}

bool set_non_blocking(int fd) {
    const int flags = fcntl(fd, F_GETFL, 0);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

bool send_byte_with_retry(int fd,
                       char value,
                       std::chrono::milliseconds timeout = 1000ms,
                       std::chrono::milliseconds step = 2ms) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        const ssize_t written = ::send(fd, &value, 1, 0);
        if (written == 1) {
            return true;
        }
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            std::this_thread::sleep_for(step);
            continue;
        }
        return false;
    }
    return false;
}

}  // namespace

int main() {
    int fds[2] = {-1, -1};
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
        std::perror("[T83] socketpair");
        return 1;
    }
    if (!set_non_blocking(fds[0]) || !set_non_blocking(fds[1])) {
        std::cerr << "[T83] failed to set non-blocking mode\n";
        close(fds[0]);
        close(fds[1]);
        return 1;
    }

    TestScheduler scheduler;
    scheduler.start();

    SharedState state(fds[0]);
    schedule_task(scheduler, bidirectional_task(&state));

    const bool first_in_position = wait_until([&]() {
        return state.bidirectional_await_bound.load(std::memory_order_acquire);
    });
    if (!first_in_position) {
        std::cerr << "[T83] bidirectional owner did not reach await point\n";
        scheduler.stop();
        close(fds[0]);
        close(fds[1]);
        return 1;
    }

    schedule_task(scheduler, second_single_direction_task(&state));
    const bool second_failed_fast = wait_until([&]() {
        return state.second_done.load(std::memory_order_acquire);
    }, 150ms);

    constexpr char kReadPayload = 'i';
    if (!send_byte_with_retry(fds[1], kReadPayload)) {
        std::cerr << "[T83] failed to send wake payload\n";
        scheduler.stop();
        close(fds[0]);
        close(fds[1]);
        return 1;
    }

    const bool first_completed = wait_until([&]() {
        return state.bidirectional_done.load(std::memory_order_acquire);
    });

    scheduler.stop();
    close(fds[0]);
    close(fds[1]);

    if (!second_failed_fast) {
        std::cerr << "[T83] single-direction sequence was not rejected immediately\n";
        return 1;
    }
    if (state.second_error.load(std::memory_order_acquire) != static_cast<int>(kNotReady)) {
        std::cerr << "[T83] single-direction sequence error mismatch: "
                  << state.second_error.load(std::memory_order_acquire) << "\n";
        return 1;
    }
    if (!first_completed) {
        std::cerr << "[T83] bidirectional owner did not complete\n";
        return 1;
    }
    if (state.bidirectional_error.load(std::memory_order_acquire) != 0) {
        std::cerr << "[T83] bidirectional owner error: "
                  << state.bidirectional_error.load(std::memory_order_acquire) << "\n";
        return 1;
    }
    if (state.bidirectional_value.load(std::memory_order_acquire) != 1) {
        std::cerr << "[T83] bidirectional owner byte count mismatch: "
                  << state.bidirectional_value.load(std::memory_order_acquire) << "\n";
        return 1;
    }

    std::cout << "T83-SequenceBidirectionalExclusive PASS\n";
    return 0;
}
