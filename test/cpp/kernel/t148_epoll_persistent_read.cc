/**
 * @file t148_epoll_persistent_read.cc
 * @brief 验证 epoll readv 完成后保留 READ 注册，并由乐观读兜住被忽略的边沿。
 */

#include <galay/cpp/galay-kernel/core/awaitable.h>
#include <galay/cpp/galay-kernel/core/task.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <fcntl.h>
#include <iostream>
#include <optional>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

#ifdef USE_EPOLL
#include <galay/cpp/galay-kernel/core/epoll_scheduler.h>
#include <sys/epoll.h>
#endif

using namespace galay::kernel;
using namespace std::chrono_literals;

#ifdef USE_EPOLL
namespace {

class TestEpollScheduler final : public EpollScheduler {
public:
    int epollFd() const {
        return m_reactor.getPollHandle().fd;
    }
};

struct ReadState {
    explicit ReadState(int fd)
        : controller(GHandle{.fd = fd}) {}

    IOController controller;
    std::atomic<bool> suspend_done{false};
    std::atomic<bool> done{false};
    std::atomic<int> value{0};
    std::atomic<int> error{0};
};

struct ReadvMachine {
    using result_type = std::expected<size_t, IOError>;
    static constexpr auto kSequenceOwnerDomain = SequenceOwnerDomain::Read;

    explicit ReadvMachine(char* buffer)
        : iovecs{{.iov_base = buffer, .iov_len = 1}} {}

    MachineAction<result_type> advance() {
        if (result.has_value()) {
            return MachineAction<result_type>::complete(std::move(*result));
        }
        if (!waiting) {
            waiting = true;
            return MachineAction<result_type>::waitReadv(iovecs, 1);
        }
        return MachineAction<result_type>::fail(IOError(kParamInvalid, 0));
    }

    void onRead(result_type read_result) {
        result = std::move(read_result);
    }

    void onWrite(result_type write_result) {
        result = write_result
            ? result_type(std::unexpected(IOError(kParamInvalid, 0)))
            : result_type(std::unexpected(write_result.error()));
    }

    iovec iovecs[1];
    std::optional<result_type> result;
    bool waiting = false;
};

template <typename InnerAwaitable>
struct SuspendProbeAwaitable {
    InnerAwaitable inner;
    std::atomic<bool>* suspend_done = nullptr;

    bool await_ready() {
        return inner.await_ready();
    }

    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> handle) {
        const bool should_suspend = inner.await_suspend(handle);
        if (suspend_done != nullptr) {
            suspend_done->store(true, std::memory_order_release);
        }
        return should_suspend;
    }

    auto await_resume() {
        return inner.await_resume();
    }
};

Task<void> readOne(ReadState* state) {
    char byte = 0;
    auto operation = AwaitableBuilder<ReadvMachine::result_type>::fromStateMachine(
                         &state->controller,
                         ReadvMachine(&byte))
                         .build();
    SuspendProbeAwaitable<decltype(operation)> awaitable{
        .inner = std::move(operation),
        .suspend_done = &state->suspend_done,
    };
    auto result = co_await awaitable;
    state->value.store(result ? static_cast<int>(result.value()) : -1,
                       std::memory_order_release);
    state->error.store(result ? 0 : static_cast<int>(result.error().code()),
                       std::memory_order_release);
    state->done.store(true, std::memory_order_release);
}

bool waitUntil(auto&& predicate,
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

bool setNonBlocking(int fd) {
    const int flags = fcntl(fd, F_GETFL, 0);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

bool sendByte(int fd, char byte) {
    while (true) {
        const ssize_t written = ::send(fd, &byte, 1, 0);
        if (written == 1) {
            return true;
        }
        if (written < 0 && errno == EINTR) {
            continue;
        }
        return false;
    }
}

bool closeFd(int fd) {
    if (::close(fd) == 0) {
        return true;
    }
    std::cerr << "[T148] close failed for fd=" << fd << ", errno=" << errno << '\n';
    return false;
}

}  // namespace
#endif

int main() {
#ifndef USE_EPOLL
    std::cout << "T148-EpollPersistentRead SKIP\n";
    return 0;
#else
    int fds[2] = {-1, -1};
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
        std::cerr << "[T148] socketpair failed, errno=" << errno << '\n';
        return 1;
    }
    if (!setNonBlocking(fds[0]) || !setNonBlocking(fds[1])) {
        std::cerr << "[T148] failed to set non-blocking mode\n";
        const bool first_closed = closeFd(fds[0]);
        const bool second_closed = closeFd(fds[1]);
        return first_closed && second_closed ? 1 : 2;
    }

    ReadState state(fds[0]);
    TestEpollScheduler scheduler;
    auto started = scheduler.start();
    if (!started) {
        std::cerr << "[T148] scheduler start failed: " << started.error().message() << '\n';
        const bool first_closed = closeFd(fds[0]);
        const bool second_closed = closeFd(fds[1]);
        return first_closed && second_closed ? 1 : 2;
    }

    bool passed = true;
    if (!scheduleTask(scheduler, readOne(&state))) {
        std::cerr << "[T148] failed to schedule first readv\n";
        passed = false;
    }
    if (passed && !waitUntil([&]() {
            return state.suspend_done.load(std::memory_order_acquire);
        })) {
        std::cerr << "[T148] first readv did not reach await_suspend\n";
        passed = false;
    }
    if (passed && !sendByte(fds[1], 'a')) {
        std::cerr << "[T148] failed to send first byte\n";
        passed = false;
    }
    if (passed && !waitUntil([&]() {
            return state.done.load(std::memory_order_acquire);
        })) {
        std::cerr << "[T148] first readv did not complete\n";
        passed = false;
    }
    if (passed && (state.value.load(std::memory_order_acquire) != 1 ||
                   state.error.load(std::memory_order_acquire) != 0)) {
        std::cerr << "[T148] first readv result mismatch\n";
        passed = false;
    }

    scheduler.stop();
    const int epoll_fd = scheduler.epollFd();
    epoll_event duplicate{};
    duplicate.events = EPOLLIN | EPOLLET;
    errno = 0;
    const int add_result = epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fds[0], &duplicate);
    if (passed && (add_result != -1 || errno != EEXIST)) {
        std::cerr << "[T148] readv completion removed persistent EPOLLIN registration\n";
        passed = false;
    }

    if (passed && !sendByte(fds[1], 'b')) {
        std::cerr << "[T148] failed to send lost-edge byte\n";
        passed = false;
    }
    if (passed) {
        bool discarded_socket_edge = false;
        const auto deadline = std::chrono::steady_clock::now() + 1000ms;
        while (!discarded_socket_edge && std::chrono::steady_clock::now() < deadline) {
            epoll_event event{};
            const int ready = epoll_wait(epoll_fd, &event, 1, 20);
            if (ready == 1 && event.data.ptr != nullptr && (event.events & EPOLLIN) != 0) {
                discarded_socket_edge = true;
            } else if (ready < 0 && errno != EINTR) {
                break;
            }
        }
        if (!discarded_socket_edge) {
            std::cerr << "[T148] failed to observe and discard the no-awaitable EPOLLIN edge\n";
            passed = false;
        }
    }

    state.suspend_done.store(false, std::memory_order_release);
    state.done.store(false, std::memory_order_release);
    state.value.store(0, std::memory_order_release);
    state.error.store(0, std::memory_order_release);
    if (passed) {
        started = scheduler.start();
        if (!started) {
            std::cerr << "[T148] scheduler restart failed: " << started.error().message() << '\n';
            passed = false;
        }
    }
    if (passed && !scheduleTask(scheduler, readOne(&state))) {
        std::cerr << "[T148] failed to schedule lost-edge fallback readv\n";
        passed = false;
    }
    if (passed && !waitUntil([&]() {
            return state.done.load(std::memory_order_acquire);
        })) {
        std::cerr << "[T148] optimistic readv did not recover the discarded edge\n";
        passed = false;
    }
    if (passed && (state.value.load(std::memory_order_acquire) != 1 ||
                   state.error.load(std::memory_order_acquire) != 0)) {
        std::cerr << "[T148] lost-edge fallback result mismatch\n";
        passed = false;
    }

    scheduler.stop();
    const bool first_closed = closeFd(fds[0]);
    const bool second_closed = closeFd(fds[1]);
    if (!first_closed || !second_closed) {
        passed = false;
    }

    if (!passed) {
        return 1;
    }
    std::cout << "T148-EpollPersistentRead PASS\n";
    return 0;
#endif
}
