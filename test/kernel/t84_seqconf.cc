/**
 * @file t84_seqconf.cc
 * @brief 用途：验证同一 controller 上已有 read sequence owner 时，普通 recv awaitable 会被立即拒绝。
 * 关键覆盖点：`suspendRegisteredAwaitable(...)` 与 sequence owner 的同方向互斥策略。
 * 通过条件：read sequence 正常完成；普通 recv 在同方向上立即返回 `kNotReady`。
 */

#include <galay/cpp/galay-kernel/core/awaitable.h>
#include <galay/cpp/galay-kernel/core/task.h>
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
    std::atomic<bool> sequence_await_bound{false};
    std::atomic<bool> sequence_done{false};
    std::atomic<bool> ordinary_done{false};
    std::atomic<int> sequence_value{0};
    std::atomic<int> ordinary_error{0};
};

Task<void> sequenceTask(SharedState* state) {
    ReadOnlyFlow flow(&state->sequence_await_bound);
    auto awaitable = AwaitableBuilder<SequenceResult, 4, ReadOnlyFlow>(&state->controller, flow)
        .recv<&ReadOnlyFlow::onRecv>(&flow.m_byte, 1)
        .build();
    auto result = co_await awaitable;
    state->sequence_value.store(result ? static_cast<int>(result.value()) : -1, std::memory_order_release);
    state->sequence_done.store(true, std::memory_order_release);
}

Task<void> ordinaryRecvTask(SharedState* state) {
    char byte = 0;
    RecvAwaitable awaitable(&state->controller, &byte, 1);
    auto result = co_await awaitable;
    state->ordinary_error.store(result ? 0 : static_cast<int>(result.error().code()), std::memory_order_release);
    state->ordinary_done.store(true, std::memory_order_release);
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
        std::perror("[T84] socketpair");
        return 1;
    }
    if (!setNonBlocking(fds[0]) || !setNonBlocking(fds[1])) {
        std::cerr << "[T84] failed to set non-blocking mode\n";
        close(fds[0]);
        close(fds[1]);
        return 1;
    }

    TestScheduler scheduler;
    scheduler.start();

    SharedState state(fds[0]);
    scheduleTask(scheduler, sequenceTask(&state));

    const bool sequence_in_position = waitUntil([&]() {
        return state.sequence_await_bound.load(std::memory_order_acquire);
    });
    if (!sequence_in_position) {
        std::cerr << "[T84] read sequence did not reach await point\n";
        scheduler.stop();
        close(fds[0]);
        close(fds[1]);
        return 1;
    }

    scheduleTask(scheduler, ordinaryRecvTask(&state));

    const bool ordinary_failed_fast = waitUntil([&]() {
        return state.ordinary_done.load(std::memory_order_acquire);
    }, 150ms);

    constexpr char kPayload = 'q';
    if (!sendByteWithRetry(fds[1], kPayload)) {
        std::cerr << "[T84] failed to send wake payload\n";
        scheduler.stop();
        close(fds[0]);
        close(fds[1]);
        return 1;
    }

    const bool sequence_completed = waitUntil([&]() {
        return state.sequence_done.load(std::memory_order_acquire);
    });

    scheduler.stop();
    close(fds[0]);
    close(fds[1]);

    if (!sequence_completed) {
        std::cerr << "[T84] read sequence did not complete\n";
        return 1;
    }
    if (state.sequence_value.load(std::memory_order_acquire) != 1) {
        std::cerr << "[T84] read sequence received unexpected byte count: "
                  << state.sequence_value.load(std::memory_order_acquire) << "\n";
        return 1;
    }
    if (!ordinary_failed_fast) {
        std::cerr << "[T84] ordinary recv was not rejected immediately\n";
        return 1;
    }
    if (state.ordinary_error.load(std::memory_order_acquire) != static_cast<int>(kNotReady)) {
        std::cerr << "[T84] ordinary recv error mismatch: "
                  << state.ordinary_error.load(std::memory_order_acquire) << "\n";
        return 1;
    }

    std::cout << "T84-SequenceOrdinaryConflict PASS\n";
    return 0;
}
