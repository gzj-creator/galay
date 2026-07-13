/**
 * @file t146_kqueue_persistent_read.cc
 * @brief 验证 kqueue 普通 recv 完成后保留 READ 注册，并可继续完成下一次 recv。
 */

#include <galay/cpp/galay-kernel/core/awaitable.h>
#include <galay/cpp/galay-kernel/core/task.h>

#include "test/cpp/common/sched_access.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <fcntl.h>
#include <iostream>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

#ifdef USE_KQUEUE
#include <galay/cpp/galay-kernel/core/kqueue_scheduler.h>
#include <sys/event.h>
#endif

using namespace galay::kernel;
using namespace std::chrono_literals;

#ifdef USE_KQUEUE
namespace {

struct RecvState {
    explicit RecvState(int fd)
        : controller(GHandle{.fd = fd}) {}

    IOController controller;
    std::atomic<bool> suspend_done{false};
    std::atomic<bool> done{false};
    std::atomic<int> value{0};
    std::atomic<int> error{0};
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

Task<void> recvOne(RecvState* state) {
    char byte = 0;
    SuspendProbeAwaitable<RecvAwaitable> awaitable{
        .inner = RecvAwaitable(&state->controller, &byte, 1),
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

bool removeReadRegistration(int kqueue_fd, int fd) {
    struct kevent change{};
    EV_SET(&change, fd, EVFILT_READ, EV_DELETE, 0, 0, nullptr);
    return kevent(kqueue_fd, &change, 1, nullptr, 0, nullptr) == 0;
}

bool closeFd(int fd) {
    if (::close(fd) == 0) {
        return true;
    }
    std::cerr << "[T146] close failed for fd=" << fd << ", errno=" << errno << '\n';
    return false;
}

}  // namespace
#endif

int main() {
#ifndef USE_KQUEUE
    std::cout << "T146-KqueuePersistentRead SKIP\n";
    return 0;
#else
    int persistent_fds[2] = {-1, -1};
    int repeat_fds[2] = {-1, -1};
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, persistent_fds) != 0 ||
        socketpair(AF_UNIX, SOCK_STREAM, 0, repeat_fds) != 0) {
        std::cerr << "[T146] socketpair failed, errno=" << errno << '\n';
        if (persistent_fds[0] >= 0) {
            const bool first_closed = closeFd(persistent_fds[0]);
            const bool second_closed = closeFd(persistent_fds[1]);
            if (!first_closed || !second_closed) {
                return 2;
            }
        }
        return 1;
    }
    if (!setNonBlocking(persistent_fds[0]) || !setNonBlocking(persistent_fds[1]) ||
        !setNonBlocking(repeat_fds[0]) || !setNonBlocking(repeat_fds[1])) {
        std::cerr << "[T146] failed to set non-blocking mode\n";
        const bool first_closed = closeFd(persistent_fds[0]);
        const bool second_closed = closeFd(persistent_fds[1]);
        const bool third_closed = closeFd(repeat_fds[0]);
        const bool fourth_closed = closeFd(repeat_fds[1]);
        return first_closed && second_closed && third_closed && fourth_closed ? 1 : 2;
    }

    RecvState persistent_state(persistent_fds[0]);
    RecvState repeat_state(repeat_fds[0]);
    KqueueScheduler scheduler;
    auto started = scheduler.start();
    if (!started) {
        std::cerr << "[T146] scheduler start failed: " << started.error().message() << '\n';
        const bool first_closed = closeFd(persistent_fds[0]);
        const bool second_closed = closeFd(persistent_fds[1]);
        const bool third_closed = closeFd(repeat_fds[0]);
        const bool fourth_closed = closeFd(repeat_fds[1]);
        return first_closed && second_closed && third_closed && fourth_closed ? 1 : 2;
    }

    const int kqueue_fd = SchedulerTestAccess::kqueueFd(scheduler);
    bool passed = true;

    if (!scheduleTask(scheduler, recvOne(&persistent_state))) {
        std::cerr << "[T146] failed to schedule first recv\n";
        passed = false;
    }
    if (passed && !waitUntil([&]() {
            return persistent_state.suspend_done.load(std::memory_order_acquire);
        })) {
        std::cerr << "[T146] first recv did not reach await_suspend\n";
        passed = false;
    }
    if (passed && !sendByte(persistent_fds[1], 'a')) {
        std::cerr << "[T146] failed to send first byte\n";
        passed = false;
    }
    if (passed && !waitUntil([&]() {
            return persistent_state.done.load(std::memory_order_acquire);
        })) {
        std::cerr << "[T146] first recv did not complete\n";
        passed = false;
    }
    if (passed && (persistent_state.value.load(std::memory_order_acquire) != 1 ||
                   persistent_state.error.load(std::memory_order_acquire) != 0)) {
        std::cerr << "[T146] first recv result mismatch\n";
        passed = false;
    }
    if (passed && !removeReadRegistration(kqueue_fd, persistent_fds[0])) {
        std::cerr << "[T146] recv completion removed persistent EVFILT_READ\n";
        passed = false;
    }

    if (passed && !scheduleTask(scheduler, recvOne(&repeat_state))) {
        std::cerr << "[T146] failed to schedule second recv\n";
        passed = false;
    }
    if (passed && !waitUntil([&]() {
            return repeat_state.suspend_done.load(std::memory_order_acquire);
        })) {
        std::cerr << "[T146] second recv did not reach await_suspend\n";
        passed = false;
    }
    if (passed && !sendByte(repeat_fds[1], 'b')) {
        std::cerr << "[T146] failed to send second byte\n";
        passed = false;
    }
    if (passed && !waitUntil([&]() {
            return repeat_state.done.load(std::memory_order_acquire);
        })) {
        std::cerr << "[T146] second recv did not complete\n";
        passed = false;
    }
    if (passed && (repeat_state.value.load(std::memory_order_acquire) != 1 ||
                   repeat_state.error.load(std::memory_order_acquire) != 0)) {
        std::cerr << "[T146] second recv result mismatch\n";
        passed = false;
    }
    repeat_state.suspend_done.store(false, std::memory_order_release);
    repeat_state.done.store(false, std::memory_order_release);
    repeat_state.value.store(0, std::memory_order_release);
    repeat_state.error.store(0, std::memory_order_release);
    if (passed && !scheduleTask(scheduler, recvOne(&repeat_state))) {
        std::cerr << "[T146] failed to schedule third recv\n";
        passed = false;
    }
    if (passed && !waitUntil([&]() {
            return repeat_state.suspend_done.load(std::memory_order_acquire);
        })) {
        std::cerr << "[T146] third recv did not reach await_suspend\n";
        passed = false;
    }
    if (passed && !sendByte(repeat_fds[1], 'c')) {
        std::cerr << "[T146] failed to send third byte\n";
        passed = false;
    }
    if (passed && !waitUntil([&]() {
            return repeat_state.done.load(std::memory_order_acquire);
        })) {
        std::cerr << "[T146] third recv did not complete\n";
        passed = false;
    }
    if (passed && (repeat_state.value.load(std::memory_order_acquire) != 1 ||
                   repeat_state.error.load(std::memory_order_acquire) != 0)) {
        std::cerr << "[T146] third recv result mismatch\n";
        passed = false;
    }

    scheduler.stop();
    const bool first_closed = closeFd(persistent_fds[0]);
    const bool second_closed = closeFd(persistent_fds[1]);
    const bool third_closed = closeFd(repeat_fds[0]);
    const bool fourth_closed = closeFd(repeat_fds[1]);
    if (!first_closed || !second_closed || !third_closed || !fourth_closed) {
        passed = false;
    }

    if (!passed) {
        return 1;
    }
    std::cout << "T146-KqueuePersistentRead PASS\n";
    return 0;
#endif
}
