/**
 * @file t82_seqsplit.cc
 * @brief 用途：定义目标行为——同一 IOController 上 read-only 与 write-only sequence 可并发成功。
 * 关键覆盖点：sequence owner 的读写双工拆分能力。
 * 通过条件：读序列和写序列都能并发挂起并最终成功返回 1 字节。
 */

#include "kernel/kernel/awaitable.h"
#include "kernel/kernel/task.h"
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <expected>
#include <fcntl.h>
#include <iostream>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

#ifdef USE_IOURING
#include "kernel/kernel/uring_scheduler.h"
using TestScheduler = galay::kernel::IOUringScheduler;
#elif defined(USE_EPOLL)
#include "kernel/kernel/epoll_scheduler.h"
using TestScheduler = galay::kernel::EpollScheduler;
#elif defined(USE_KQUEUE)
#include "kernel/kernel/kqueue_scheduler.h"
using TestScheduler = galay::kernel::KqueueScheduler;
#endif

using namespace galay::kernel;
using namespace std::chrono_literals;

namespace {

using SequenceResult = std::expected<size_t, IOError>;

struct ReadOnlyFlow {
    void onRecv(SequenceOps<SequenceResult, 4>& ops, RecvIOContext& ctx) {
        ops.complete(std::move(ctx.m_result));
    }

    char m_byte = 0;
};

struct WriteOnlyFlow {
    void onSend(SequenceOps<SequenceResult, 4>& ops, SendIOContext& ctx) {
        ops.complete(std::move(ctx.m_result));
    }

    const char m_byte = 'w';
};

struct SharedState {
    explicit SharedState(int fd)
        : controller(GHandle{.fd = fd}) {}

    IOController controller;
    std::atomic<bool> read_suspend_done{false};
    std::atomic<bool> write_suspend_done{false};
    std::atomic<bool> read_done{false};
    std::atomic<bool> write_done{false};
    std::atomic<int> read_value{0};
    std::atomic<int> write_value{0};
    std::atomic<int> read_error{0};
    std::atomic<int> write_error{0};
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

Task<void> readTask(SharedState* state) {
    ReadOnlyFlow flow;
    auto inner = AwaitableBuilder<SequenceResult, 4, ReadOnlyFlow>(&state->controller, flow)
        .recv<&ReadOnlyFlow::onRecv>(&flow.m_byte, 1)
        .build();
    SuspendProbeAwaitable<decltype(inner)> awaitable{
        .inner = std::move(inner),
        .suspend_done = &state->read_suspend_done,
    };
    auto result = co_await awaitable;
    state->read_value.store(result ? static_cast<int>(result.value()) : -1, std::memory_order_release);
    state->read_error.store(result ? 0 : static_cast<int>(result.error().code()), std::memory_order_release);
    state->read_done.store(true, std::memory_order_release);
}

Task<void> writeTask(SharedState* state) {
    WriteOnlyFlow flow;
    auto inner = AwaitableBuilder<SequenceResult, 4, WriteOnlyFlow>(&state->controller, flow)
        .send<&WriteOnlyFlow::onSend>(&flow.m_byte, 1)
        .build();
    SuspendProbeAwaitable<decltype(inner)> awaitable{
        .inner = std::move(inner),
        .suspend_done = &state->write_suspend_done,
    };
    auto result = co_await awaitable;
    state->write_value.store(result ? static_cast<int>(result.value()) : -1, std::memory_order_release);
    state->write_error.store(result ? 0 : static_cast<int>(result.error().code()), std::memory_order_release);
    state->write_done.store(true, std::memory_order_release);
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

bool sendByteWithRetry(int fd,
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

bool drainPeerUntilWriteCompletes(SharedState* state,
                                  int fd,
                                  std::chrono::milliseconds timeout = 1000ms,
                                  std::chrono::milliseconds step = 2ms) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    char drained = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        if (state->write_done.load(std::memory_order_acquire)) {
            return true;
        }

        const ssize_t read_n = ::recv(fd, &drained, 1, 0);
        if (read_n == 1) {
            continue;
        }
        if (read_n < 0 && errno == EINTR) {
            continue;
        }
        if (read_n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            std::this_thread::sleep_for(step);
            continue;
        }
        return false;
    }
    return state->write_done.load(std::memory_order_acquire);
}

bool fillSendBuffer(int fd) {
    std::array<char, 4096> payload{};
    payload.fill('b');
    while (true) {
        const ssize_t written = ::send(fd, payload.data(), payload.size(), 0);
        if (written > 0) {
            continue;
        }
        if (written == 0) {
            return false;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return true;
        }
        return false;
    }
}

}  // namespace

int main() {
    int fds[2] = {-1, -1};
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
        std::perror("[T82] socketpair");
        return 1;
    }
    if (!setNonBlocking(fds[0]) || !setNonBlocking(fds[1])) {
        std::cerr << "[T82] failed to set non-blocking mode\n";
        close(fds[0]);
        close(fds[1]);
        return 1;
    }
    if (!fillSendBuffer(fds[0])) {
        std::cerr << "[T82] failed to pre-fill socket send buffer\n";
        close(fds[0]);
        close(fds[1]);
        return 1;
    }

    TestScheduler scheduler;
    scheduler.start();

    SharedState state(fds[0]);
    scheduleTask(scheduler, readTask(&state));
    scheduleTask(scheduler, writeTask(&state));

    const bool suspend_barrier_ready = waitUntil([&]() {
        return state.read_suspend_done.load(std::memory_order_acquire) &&
               state.write_suspend_done.load(std::memory_order_acquire);
    });
    if (!suspend_barrier_ready) {
        std::cerr << "[T82] read/write inner await_suspend did not both run\n";
        scheduler.stop();
        close(fds[0]);
        close(fds[1]);
        return 1;
    }

    constexpr char kReadPayload = 'r';
    if (!sendByteWithRetry(fds[1], kReadPayload)) {
        std::cerr << "[T82] failed to inject read payload\n";
        scheduler.stop();
        close(fds[0]);
        close(fds[1]);
        return 1;
    }

    if (!drainPeerUntilWriteCompletes(&state, fds[1])) {
        std::cerr << "[T82] failed to drain peer buffer until write completion\n";
        scheduler.stop();
        close(fds[0]);
        close(fds[1]);
        return 1;
    }

    const bool read_completed = waitUntil([&]() {
        return state.read_done.load(std::memory_order_acquire);
    });
    const bool write_completed = waitUntil([&]() {
        return state.write_done.load(std::memory_order_acquire);
    });

    scheduler.stop();
    close(fds[0]);
    close(fds[1]);

    if (!read_completed) {
        std::cerr << "[T82] read sequence did not complete\n";
        return 1;
    }
    if (!write_completed) {
        std::cerr << "[T82] write sequence did not complete\n";
        return 1;
    }
    if (state.read_error.load(std::memory_order_acquire) != 0) {
        std::cerr << "[T82] read sequence error: "
                  << state.read_error.load(std::memory_order_acquire) << "\n";
        return 1;
    }
    if (state.write_error.load(std::memory_order_acquire) != 0) {
        std::cerr << "[T82] write sequence error: "
                  << state.write_error.load(std::memory_order_acquire) << "\n";
        return 1;
    }
    if (state.read_value.load(std::memory_order_acquire) != 1) {
        std::cerr << "[T82] read sequence byte count mismatch: "
                  << state.read_value.load(std::memory_order_acquire) << "\n";
        return 1;
    }
    if (state.write_value.load(std::memory_order_acquire) != 1) {
        std::cerr << "[T82] write sequence byte count mismatch: "
                  << state.write_value.load(std::memory_order_acquire) << "\n";
        return 1;
    }

    std::cout << "T82-SequenceDuplexSplit PASS\n";
    return 0;
}
