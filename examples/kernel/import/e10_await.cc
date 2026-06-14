/**
 * @file e10_await.cc
 * @brief 用途：用模块导入方式演示如何通过状态机实现一个最小自定义 Awaitable。
 * 关键覆盖点：`MachineAction`、`advance()`、`onRead()`、`onWrite()`、
 * `AwaitableBuilder<Result>::fromStateMachine(...).build()`。
 * 通过条件：本地 `ping -> pong` 自闭环完成并返回 0。
 */

import galay.kernel;

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <expected>
#include <iostream>
#include <optional>
#include <string>
#include <sys/socket.h>
#include <sys/time.h>
#include <thread>
#include <unistd.h>

using namespace galay::kernel;
using namespace std::chrono_literals;

namespace {

using ExampleResult = std::expected<std::string, IOError>;

struct PingPongMachine {
    using result_type = ExampleResult;

    MachineAction<result_type> advance() {
        if (m_result.has_value()) {
            return MachineAction<result_type>::complete(std::move(*m_result));
        }

        switch (m_phase) {
        case Phase::kReadPing:
            return MachineAction<result_type>::waitRead(
                m_ping.data() + m_ping_received,
                m_ping.size() - m_ping_received
            );
        case Phase::kWritePong:
            return MachineAction<result_type>::waitWrite(
                m_pong.data() + m_pong_sent,
                m_pong.size() - m_pong_sent
            );
        case Phase::kDone:
            return MachineAction<result_type>::fail(IOError(kParamInvalid, 0));
        }
        return MachineAction<result_type>::fail(IOError(kParamInvalid, 0));
    }

    void onRead(std::expected<size_t, IOError> result) {
        if (!result) {
            m_result = std::unexpected(result.error());
            m_phase = Phase::kDone;
            return;
        }

        m_ping_received += result.value();
        if (m_ping_received < m_ping.size()) {
            return;
        }

        if (std::string(m_ping.data(), m_ping.size()) != "ping") {
            m_result = std::unexpected(IOError(kReadFailed, 0));
            m_phase = Phase::kDone;
            return;
        }

        m_phase = Phase::kWritePong;
    }

    void onWrite(std::expected<size_t, IOError> result) {
        if (!result) {
            m_result = std::unexpected(result.error());
            m_phase = Phase::kDone;
            return;
        }

        m_pong_sent += result.value();
        if (m_pong_sent < m_pong.size()) {
            return;
        }

        m_result = std::string(m_pong.data(), m_pong.size());
        m_phase = Phase::kDone;
    }

private:
    enum class Phase {
        kReadPing,
        kWritePong,
        kDone,
    };

    Phase m_phase = Phase::kReadPing;
    std::optional<ExampleResult> m_result;
    std::array<char, 4> m_ping{};
    size_t m_ping_received = 0;
    std::array<char, 4> m_pong{'p', 'o', 'n', 'g'};
    size_t m_pong_sent = 0;
};

struct ExampleState {
    std::atomic<bool> done{false};
    std::atomic<bool> machine_ok{false};
    std::atomic<bool> peer_ok{false};
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

void setRecvTimeout(int fd, int milliseconds) {
    timeval tv{};
    tv.tv_sec = milliseconds / 1000;
    tv.tv_usec = (milliseconds % 1000) * 1000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

Task<void> customAwaitableTask(ExampleState* state, int fd) {
    IOController controller(GHandle{.fd = fd});
    auto awaitable = AwaitableBuilder<ExampleResult>::fromStateMachine(
        &controller,
        PingPongMachine{}
    ).build();

    auto result = co_await awaitable;
    state->machine_ok.store(
        result.has_value() && result.value() == "pong",
        std::memory_order_release
    );
    state->done.store(true, std::memory_order_release);
}

}  // namespace

int main() {
    int fds[2] = {-1, -1};
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
        std::cerr << "socketpair failed: " << std::strerror(errno) << "\n";
        return 1;
    }

    setRecvTimeout(fds[1], 1000);

    ExampleState state;
    std::thread peer([&]() {
        constexpr char kPing[] = "ping";
        char pong[4]{};

        if (sendAll(fds[1], kPing, sizeof(kPing) - 1) &&
            recvExact(fds[1], pong, sizeof(pong)) &&
            std::string(pong, sizeof(pong)) == "pong") {
            state.peer_ok.store(true, std::memory_order_release);
        }
    });

    Runtime runtime = RuntimeBuilder()
        .ioSchedulerCount(1)
        .computeSchedulerCount(1)
        .build();
    runtime.start();

    scheduleTask(runtime.getNextIOScheduler(), customAwaitableTask(&state, fds[0]));

    const bool completed = waitUntil(state.done, 2000ms);

    runtime.stop();
    close(fds[0]);
    close(fds[1]);
    peer.join();

    if (!completed) {
        std::cerr << "custom awaitable import example timed out\n";
        return 1;
    }
    if (!state.machine_ok.load(std::memory_order_acquire) ||
        !state.peer_ok.load(std::memory_order_acquire)) {
        std::cerr << "custom awaitable import example failed\n";
        return 1;
    }

    std::cout << "custom awaitable import example passed\n";
    return 0;
}
