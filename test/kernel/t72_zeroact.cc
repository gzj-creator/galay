/**
 * @file t72_zeroact.cc
 * @brief 用途：验证状态机允许零长度读写动作，并沿用底层 IO 处理的 0 字节语义。
 * 关键覆盖点：`WaitRead(..., 0)`、`WaitWrite(..., 0)`。
 * 通过条件：状态机成功完成零长度读写并返回 0。
 */

#include <galay/cpp/galay-kernel/core/awaitable.h>
#include <galay/cpp/galay-kernel/core/task.h>
#include <array>
#include <atomic>
#include <chrono>
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

using ZeroResult = std::expected<size_t, IOError>;

struct ZeroLengthMachine {
    using result_type = ZeroResult;

    MachineAction<result_type> advance() {
        if (m_result.has_value()) {
            return MachineAction<result_type>::complete(std::move(*m_result));
        }

        switch (m_phase) {
        case Phase::ZeroRead:
            return MachineAction<result_type>::waitRead(m_read_buffer.data(), 0);
        case Phase::ZeroWrite:
            return MachineAction<result_type>::waitWrite(m_write_buffer.data(), 0);
        case Phase::Done:
            return MachineAction<result_type>::fail(IOError(kParamInvalid, 0));
        }
        return MachineAction<result_type>::fail(IOError(kParamInvalid, 0));
    }

    void onRead(std::expected<size_t, IOError> result) {
        if (!result || result.value() != 0) {
            m_result = std::unexpected(result ? IOError(kReadFailed, 0) : result.error());
            m_phase = Phase::Done;
            return;
        }
        m_phase = Phase::ZeroWrite;
    }

    void onWrite(std::expected<size_t, IOError> result) {
        if (!result || result.value() != 0) {
            m_result = std::unexpected(result ? IOError(kSendFailed, 0) : result.error());
            m_phase = Phase::Done;
            return;
        }
        m_result = 0;
        m_phase = Phase::Done;
    }

private:
    enum class Phase {
        ZeroRead,
        ZeroWrite,
        Done,
    };

    Phase m_phase = Phase::ZeroRead;
    std::optional<ZeroResult> m_result;
    std::array<char, 1> m_read_buffer{};
    std::array<char, 1> m_write_buffer{};
};

struct TestState {
    std::atomic<bool> done{false};
    std::atomic<bool> success{false};
};

Task<void> zeroLengthTask(TestState* state, int fd) {
    IOController controller(GHandle{.fd = fd});
    StateMachineAwaitable<ZeroLengthMachine> awaitable(&controller, ZeroLengthMachine{});

    auto result = co_await awaitable;
    state->success.store(result.has_value() && result.value() == 0, std::memory_order_release);
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
    int fds[2] = {-1, -1};
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
        std::cerr << "[T72] socketpair failed\n";
        return 1;
    }

    TestScheduler scheduler;
    scheduler.start();

    TestState state;
    scheduleTask(scheduler, zeroLengthTask(&state, fds[0]));

    const bool completed = waitUntil(state.done);
    scheduler.stop();
    close(fds[0]);
    close(fds[1]);

    if (!completed) {
        std::cerr << "[T72] zero-length state machine timed out\n";
        return 1;
    }
    if (!state.success.load(std::memory_order_acquire)) {
        std::cerr << "[T72] zero-length state machine result mismatch\n";
        return 1;
    }

    std::cout << "T72-StateMachineZeroLengthActions PASS\n";
    return 0;
}
