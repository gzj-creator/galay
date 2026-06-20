/**
 * @file t69_smloop.cc
 * @brief 用途：验证状态机 Awaitable 能完成 read -> write -> read 的事件切换。
 * 关键覆盖点：`StateMachineAwaitable`、`WaitRead`、`WaitWrite`、完成后恢复协程。
 * 通过条件：状态机完成完整收发环并返回预期结果。
 */

#include <galay/cpp/galay-kernel/core/awaitable.h>
#include <galay/cpp/galay-kernel/core/task.h>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <expected>
#include <iostream>
#include <string>
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

using MachineResult = std::expected<std::string, IOError>;

struct ReadWriteLoopMachine {
    using result_type = MachineResult;

    MachineAction<result_type> advance() {
        if (m_result.has_value()) {
            return MachineAction<result_type>::complete(std::move(*m_result));
        }

        switch (m_phase) {
        case Phase::ReadHello:
            return MachineAction<result_type>::waitRead(
                m_hello.data() + m_hello_received,
                m_hello.size() - m_hello_received
            );
        case Phase::TransitionToWrite:
            m_phase = Phase::WritePong;
            return MachineAction<result_type>::continue_();
        case Phase::WritePong:
            return MachineAction<result_type>::waitWrite(
                m_pong.data() + m_pong_sent,
                m_pong.size() - m_pong_sent
            );
        case Phase::ReadWorld:
            return MachineAction<result_type>::waitRead(
                m_world.data() + m_world_received,
                m_world.size() - m_world_received
            );
        case Phase::Done:
            return MachineAction<result_type>::fail(IOError(kParamInvalid, 0));
        }
        return MachineAction<result_type>::fail(IOError(kParamInvalid, 0));
    }

    void onRead(std::expected<size_t, IOError> result) {
        if (!result) {
            m_result = std::unexpected(result.error());
            m_phase = Phase::Done;
            return;
        }

        if (m_phase == Phase::ReadHello) {
            m_hello_received += result.value();
            if (m_hello_received < m_hello.size()) {
                return;
            }
            if (std::string(m_hello.data(), m_hello.size()) != "hello") {
                m_result = std::unexpected(IOError(kReadFailed, 0));
            } else {
                m_phase = Phase::TransitionToWrite;
            }
            if (m_result.has_value()) {
                m_phase = Phase::Done;
            }
            return;
        }

        if (m_phase == Phase::ReadWorld) {
            m_world_received += result.value();
            if (m_world_received < m_world.size()) {
                return;
            }
            m_result = std::string(m_world.data(), m_world.size());
            m_phase = Phase::Done;
            return;
        }

        m_result = std::unexpected(IOError(kParamInvalid, 0));
        m_phase = Phase::Done;
    }

    void onWrite(std::expected<size_t, IOError> result) {
        if (!result) {
            m_result = std::unexpected(result.error());
            m_phase = Phase::Done;
            return;
        }

        if (m_phase != Phase::WritePong) {
            m_result = std::unexpected(IOError(kSendFailed, 0));
            m_phase = Phase::Done;
            return;
        }

        m_pong_sent += result.value();
        if (m_pong_sent >= m_pong.size()) {
            m_phase = Phase::ReadWorld;
        }
    }

private:
    enum class Phase {
        ReadHello,
        TransitionToWrite,
        WritePong,
        ReadWorld,
        Done,
    };

    Phase m_phase = Phase::ReadHello;
    std::optional<MachineResult> m_result;
    std::array<char, 5> m_hello{};
    size_t m_hello_received = 0;
    std::array<char, 4> m_pong{'p', 'o', 'n', 'g'};
    size_t m_pong_sent = 0;
    std::array<char, 5> m_world{};
    size_t m_world_received = 0;
};

struct TestState {
    std::atomic<bool> done{false};
    std::atomic<bool> success{false};
};

Task<void> machineTask(TestState* state, int fd) {
    IOController controller(GHandle{.fd = fd});
    StateMachineAwaitable<ReadWriteLoopMachine> awaitable(&controller, ReadWriteLoopMachine{});

    auto result = co_await awaitable;
    state->success.store(result.has_value() && result.value() == "world", std::memory_order_release);
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

bool sendAll(int fd, const char* buffer, size_t length) {
    size_t sent = 0;
    while (sent < length) {
        const ssize_t n = ::send(fd, buffer + sent, length - sent, 0);
        if (n <= 0) {
            return false;
        }
        sent += static_cast<size_t>(n);
    }
    return true;
}

bool recvExact(int fd, char* buffer, size_t length) {
    size_t received = 0;
    while (received < length) {
        const ssize_t n = ::recv(fd, buffer + received, length - received, 0);
        if (n <= 0) {
            return false;
        }
        received += static_cast<size_t>(n);
    }
    return true;
}

}  // namespace

int main() {
    int fds[2] = {-1, -1};
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
        std::cerr << "[T69] socketpair failed: " << std::strerror(errno) << "\n";
        return 1;
    }

    TestScheduler scheduler;
    scheduler.start();

    TestState state;
    std::thread peer([&]() {
        constexpr char hello[] = "hello";
        constexpr char world[] = "world";
        char pong[4]{};

        if (sendAll(fds[1], hello, sizeof(hello) - 1) &&
            recvExact(fds[1], pong, sizeof(pong)) &&
            std::string(pong, sizeof(pong)) == "pong") {
            sendAll(fds[1], world, sizeof(world) - 1);
        }
    });

    scheduleTask(scheduler, machineTask(&state, fds[0]));

    const bool completed = waitUntil(state.done);
    scheduler.stop();
    close(fds[0]);
    peer.join();
    close(fds[1]);

    if (!completed) {
        std::cerr << "[T69] state machine timed out\n";
        return 1;
    }
    if (!state.success.load(std::memory_order_acquire)) {
        std::cerr << "[T69] state machine result mismatch\n";
        return 1;
    }

    std::cout << "T69-StateMachineReadWriteLoop PASS\n";
    return 0;
}
