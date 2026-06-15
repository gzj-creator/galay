/**
 * @file t81_seqown.cc
 * @brief 用途：验证同一方向（read/read）sequence owner 冲突时第二个会被立即拒绝。
 * 关键覆盖点：`suspendSequenceAwaitable(...)` 同方向 owner 冲突策略。
 * 通过条件：第一个 read sequence 正常完成；第二个 read sequence 立即返回 `kNotReady`。
 */

#include "galay-kernel/core/awaitable.h"
#include "galay-kernel/core/task.h"
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

using SequenceResult = std::expected<size_t, IOError>;

struct ReadOnlyFlow {
    explicit ReadOnlyFlow(std::atomic<bool>* await_bound)
        : m_await_bound(await_bound) {}

    void onAwaitContext(const AwaitContext&) {
        if (m_await_bound != nullptr) {
            m_await_bound->store(true, std::memory_order_release);
        }
    }

    void onRecv(SequenceOps<SequenceResult, 4>& ops, RecvIOContext& ctx) {
        ops.complete(std::move(ctx.m_result));
    }

    std::atomic<bool>* m_await_bound = nullptr;
    char m_byte = 0;
};

struct SharedState {
    explicit SharedState(int fd)
        : controller(GHandle{.fd = fd}) {}

    IOController controller;
    std::atomic<bool> first_await_bound{false};
    std::atomic<bool> first_done{false};
    std::atomic<bool> second_done{false};
    std::atomic<int> first_value{0};
    std::atomic<int> second_error{0};
};

Task<void> firstOwnerTask(SharedState* state) {
    ReadOnlyFlow flow(&state->first_await_bound);
    auto awaitable = AwaitableBuilder<SequenceResult, 4, ReadOnlyFlow>(&state->controller, flow)
        .recv<&ReadOnlyFlow::onRecv>(&flow.m_byte, 1)
        .build();
    auto result = co_await awaitable;
    state->first_value.store(result ? static_cast<int>(result.value()) : -1, std::memory_order_release);
    state->first_done.store(true, std::memory_order_release);
}

Task<void> secondOwnerTask(SharedState* state) {
    ReadOnlyFlow flow(nullptr);
    auto awaitable = AwaitableBuilder<SequenceResult, 4, ReadOnlyFlow>(&state->controller, flow)
        .recv<&ReadOnlyFlow::onRecv>(&flow.m_byte, 1)
        .build();
    auto result = co_await awaitable;
    state->second_error.store(result ? 0 : static_cast<int>(result.error().code()), std::memory_order_release);
    state->second_done.store(true, std::memory_order_release);
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

}  // namespace

int main() {
    int fds[2] = {-1, -1};
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
        std::perror("[T81] socketpair");
        return 1;
    }
    if (!setNonBlocking(fds[0]) || !setNonBlocking(fds[1])) {
        std::cerr << "[T81] failed to set non-blocking mode\n";
        close(fds[0]);
        close(fds[1]);
        return 1;
    }

    TestScheduler scheduler;
    scheduler.start();

    SharedState state(fds[0]);
    scheduleTask(scheduler, firstOwnerTask(&state));

    const bool first_in_position = waitUntil([&]() {
        return state.first_await_bound.load(std::memory_order_acquire);
    });
    if (!first_in_position) {
        std::cerr << "[T81] first read sequence did not reach await point\n";
        scheduler.stop();
        close(fds[0]);
        close(fds[1]);
        return 1;
    }

    scheduleTask(scheduler, secondOwnerTask(&state));

    const bool second_failed_fast = waitUntil([&]() {
        return state.second_done.load(std::memory_order_acquire);
    }, 150ms);

    constexpr char kPayload = 'z';
    if (!sendByteWithRetry(fds[1], kPayload)) {
        std::cerr << "[T81] failed to send wake payload\n";
        scheduler.stop();
        close(fds[0]);
        close(fds[1]);
        return 1;
    }

    const bool first_completed = waitUntil([&]() {
        return state.first_done.load(std::memory_order_acquire);
    });

    scheduler.stop();
    close(fds[0]);
    close(fds[1]);

    if (!first_completed) {
        std::cerr << "[T81] first sequence owner did not complete\n";
        return 1;
    }
    if (state.first_value.load(std::memory_order_acquire) != 1) {
        std::cerr << "[T81] first sequence owner read unexpected byte count: "
                  << state.first_value.load(std::memory_order_acquire) << "\n";
        return 1;
    }
    if (!second_failed_fast) {
        std::cerr << "[T81] second read sequence was not rejected immediately\n";
        return 1;
    }
    if (state.second_error.load(std::memory_order_acquire) != static_cast<int>(kNotReady)) {
        std::cerr << "[T81] second sequence owner error mismatch: "
                  << state.second_error.load(std::memory_order_acquire) << "\n";
        return 1;
    }

    std::cout << "T81-SequenceOwnerConflict PASS\n";
    return 0;
}
