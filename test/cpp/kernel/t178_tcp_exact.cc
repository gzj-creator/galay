/**
 * @file t178_tcp_exact.cc
 * @brief 验证 AsyncTcpSocket 的 readExact/writeAll 组合式流操作。
 */

#include <galay/cpp/galay-kernel/async/async_tcp.h>
#include <galay/cpp/galay-kernel/core/task.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

#ifdef USE_EPOLL
#include <galay/cpp/galay-kernel/core/epoll_scheduler.h>
#endif

using namespace galay::async;
using namespace galay::kernel;
using namespace std::chrono_literals;

#ifdef USE_EPOLL
namespace {

struct State {
    explicit State(int fd)
        : socket(GHandle{.fd = fd}) {}

    AsyncTcpSocket socket;
    std::array<char, 5> buffer{};
    std::atomic<bool> done{false};
    std::atomic<int> result{-2};
};

Task<void> readTask(State* state)
{
    auto result = co_await state->socket.readExact(
        state->buffer.data(), state->buffer.size());
    state->result.store(result ? static_cast<int>(result.value())
                               : -static_cast<int>(result.error().code()),
                        std::memory_order_release);
    state->done.store(true, std::memory_order_release);
}

Task<void> writeTask(State* state)
{
    constexpr char payload[] = "world";
    auto result = co_await state->socket.writeAll(payload, sizeof(payload) - 1);
    state->result.store(result ? static_cast<int>(result.value())
                               : -static_cast<int>(result.error().code()),
                        std::memory_order_release);
    state->done.store(true, std::memory_order_release);
}

bool setNonBlocking(int fd)
{
    const int flags = ::fcntl(fd, F_GETFL, 0);
    return flags >= 0 && ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

bool waitFor(const std::atomic<bool>& done,
             std::chrono::milliseconds timeout = 1s)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (done.load(std::memory_order_acquire)) {
            return true;
        }
        std::this_thread::sleep_for(1ms);
    }
    return done.load(std::memory_order_acquire);
}

}  // namespace
#endif

int main()
{
#ifndef USE_EPOLL
    std::cout << "T178-TcpExact SKIP (requires epoll)\n";
    return 0;
#else
    int fds[2] = {-1, -1};
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0 ||
        !setNonBlocking(fds[0]) || !setNonBlocking(fds[1])) {
        std::cerr << "[T178] failed to create non-blocking socketpair\n";
        if (fds[0] >= 0) ::close(fds[0]);
        if (fds[1] >= 0) ::close(fds[1]);
        return 1;
    }

    EpollScheduler scheduler;
    if (!scheduler.start()) {
        std::cerr << "[T178] scheduler failed to start\n";
        ::close(fds[0]);
        ::close(fds[1]);
        return 1;
    }

    State state(fds[0]);
    if (!scheduleTask(scheduler, readTask(&state))) {
        std::cerr << "[T178] failed to schedule read task\n";
        scheduler.stop();
        ::close(fds[1]);
        return 1;
    }

    constexpr char first[] = "he";
    constexpr char second[] = "llo";
    if (::send(fds[1], first, sizeof(first) - 1, 0) != 2) {
        std::cerr << "[T178] failed to send first partial frame\n";
        scheduler.stop();
        ::close(fds[1]);
        return 1;
    }
    std::this_thread::sleep_for(20ms);
    if (state.done.load(std::memory_order_acquire)) {
        std::cerr << "[T178] readExact completed before the frame was full\n";
        scheduler.stop();
        ::close(fds[1]);
        return 1;
    }
    if (::send(fds[1], second, sizeof(second) - 1, 0) != 3 ||
        !waitFor(state.done)) {
        std::cerr << "[T178] readExact did not complete after the remaining bytes\n";
        scheduler.stop();
        ::close(fds[1]);
        return 1;
    }

    if (state.result.load(std::memory_order_acquire) != 5 ||
        std::memcmp(state.buffer.data(), "hello", state.buffer.size()) != 0) {
        scheduler.stop();
        ::close(fds[1]);
        std::cerr << "[T178] exact read result mismatch\n";
        return 1;
    }

    state.done.store(false, std::memory_order_release);
    state.result.store(-2, std::memory_order_release);
    if (!scheduleTask(scheduler, writeTask(&state)) || !waitFor(state.done)) {
        std::cerr << "[T178] writeAll did not complete\n";
        scheduler.stop();
        return 1;
    }
    std::array<char, 5> written{};
    const ssize_t received = ::recv(fds[1], written.data(), written.size(), 0);
    if (state.result.load(std::memory_order_acquire) != 5 ||
        received != 5 || std::memcmp(written.data(), "world", written.size()) != 0) {
        std::cerr << "[T178] exact write result mismatch\n";
        scheduler.stop();
        return 1;
    }

    scheduler.stop();
    ::close(fds[1]);
    std::cout << "T178-TcpExact PASS\n";
    return 0;
#endif
}
