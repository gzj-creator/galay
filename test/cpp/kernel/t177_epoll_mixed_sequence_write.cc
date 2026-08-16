/**
 * @file t177_epoll_mixed_sequence_write.cc
 * @brief 验证合并的 EPOLLIN|EPOLLOUT 会同时分发普通写和只读 sequence。
 */

#include <galay/cpp/galay-kernel/core/awaitable.h>
#include <galay/cpp/galay-kernel/core/task.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <coroutine>
#include <cstdint>
#include <expected>
#include <fcntl.h>
#include <iostream>
#include <sys/socket.h>
#include <sys/uio.h>
#include <thread>
#include <unistd.h>
#include <utility>

#ifdef USE_EPOLL
#include <galay/cpp/galay-kernel/core/epoll_scheduler.h>
#endif

using namespace galay::kernel;
using namespace std::chrono_literals;

#ifdef USE_EPOLL
namespace {

using IOResult = std::expected<size_t, IOError>;

class TestEpollScheduler final : public EpollScheduler {
public:
    bool pollOnceAfterStop() {
        if (!m_worker.reopenResumeAdmission()) {
            return false;
        }
        m_reactor.poll(0, m_wake_coordinator);
        return true;
    }

    void finishManualDispatch() {
        for (int round = 0; round < 4; ++round) {
            while (m_core.hasPendingWork()) {
                (void)m_core.runReadyPass(
                    [this](TaskRef& next) { resume(next); },
                    [this](size_t drained) {
                        m_wake_coordinator.onRemoteCollected(drained);
                    });
            }
            m_reactor.poll(0, m_wake_coordinator);
        }
    }
};

struct SharedState {
    explicit SharedState(int fd)
        : controller(GHandle{.fd = fd}) {}

    IOController controller;
    std::atomic<bool> read_suspended{false};
    std::atomic<bool> write_suspended{false};
    std::atomic<bool> read_done{false};
    std::atomic<bool> write_done{false};
    std::atomic<int> read_value{0};
    std::atomic<int> write_value{0};
};

struct ReadFlow {
    void onReadv(SequenceOps<IOResult, 4>& ops, ReadvIOContext& context) {
        ops.complete(std::move(context.m_result));
    }

    char byte = 0;
    std::array<iovec, 1> iovecs{};
};

template <typename InnerAwaitable>
struct SuspendProbeAwaitable {
    InnerAwaitable inner;
    std::atomic<bool>* suspended = nullptr;

    bool await_ready() {
        return inner.await_ready();
    }

    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> handle) {
        const bool should_suspend = inner.await_suspend(handle);
        suspended->store(true, std::memory_order_release);
        return should_suspend;
    }

    auto await_resume() {
        return inner.await_resume();
    }
};

struct DirectWriteAwaitable final : WritevAwaitable {
    DirectWriteAwaitable(IOController* controller,
                         std::array<iovec, 1>& iovecs,
                         const char* data,
                         std::atomic<bool>* suspended)
        : WritevAwaitable(controller, iovecs)
        , data(data)
        , suspended(suspended) {}

    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> handle) {
        const bool should_suspend = WritevAwaitable::await_suspend(handle);
        suspended->store(true, std::memory_order_release);
        return should_suspend;
    }

    bool handleComplete(GHandle handle) override {
        const ssize_t written = ::write(handle.fd, data, 1);
        if (written == 1) {
            m_result = 1;
            return true;
        }
        if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
            return false;
        }
        m_result = std::unexpected(
            IOError(kSendFailed, written < 0 ? static_cast<uint32_t>(errno) : 0));
        return true;
    }

    const char* data;
    std::atomic<bool>* suspended;
};

Task<void> readSequence(SharedState* state) {
    ReadFlow flow;
    flow.iovecs[0] = iovec{.iov_base = &flow.byte, .iov_len = 1};
    auto inner = AwaitableBuilder<IOResult, 4, ReadFlow>(&state->controller, flow)
        .readv<&ReadFlow::onReadv>(flow.iovecs)
        .build();
    SuspendProbeAwaitable<decltype(inner)> awaitable{
        .inner = std::move(inner),
        .suspended = &state->read_suspended,
    };
    auto result = co_await awaitable;
    state->read_value.store(result ? static_cast<int>(result.value()) : -1,
                            std::memory_order_release);
    state->read_done.store(true, std::memory_order_release);
}

Task<void> simpleWrite(SharedState* state) {
    char byte = 'w';
    std::array<iovec, 1> iovecs{{iovec{
        .iov_base = &byte,
        .iov_len = 1,
    }}};
    DirectWriteAwaitable awaitable(
        &state->controller, iovecs, &byte, &state->write_suspended);
    auto result = co_await awaitable;
    state->write_value.store(result ? static_cast<int>(result.value()) : -1,
                             std::memory_order_release);
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

bool fillSendBuffer(int fd) {
    std::array<char, 4096> data{};
    while (true) {
        const ssize_t written = ::write(fd, data.data(), data.size());
        if (written > 0) {
            continue;
        }
        if (written < 0 && errno == EINTR) {
            continue;
        }
        return written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK);
    }
}

bool writeByte(int fd, char byte) {
    while (true) {
        const ssize_t written = ::write(fd, &byte, 1);
        if (written == 1) {
            return true;
        }
        if (written < 0 && errno == EINTR) {
            continue;
        }
        return false;
    }
}

bool drainSocket(int fd) {
    std::array<char, 4096> data{};
    bool drained = false;
    while (true) {
        const ssize_t read_count = ::read(fd, data.data(), data.size());
        if (read_count > 0) {
            drained = true;
            continue;
        }
        if (read_count < 0 && errno == EINTR) {
            continue;
        }
        return drained && read_count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK);
    }
}

bool closeSocket(int fd) {
    if (fd < 0 || ::close(fd) == 0) {
        return true;
    }
    std::cerr << "[T177] close failed for fd=" << fd << ", errno=" << errno << '\n';
    return false;
}

}  // namespace
#endif

int main() {
#ifndef USE_EPOLL
    std::cout << "T177-EpollMixedSequenceWrite SKIP\n";
    return 0;
#else
    int fds[2] = {-1, -1};
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0 ||
        !setNonBlocking(fds[0]) || !setNonBlocking(fds[1]) ||
        !fillSendBuffer(fds[0])) {
        std::cerr << "[T177] failed to prepare a blocked nonblocking socket\n";
        const bool first_closed = closeSocket(fds[0]);
        const bool second_closed = closeSocket(fds[1]);
        return first_closed && second_closed ? 1 : 2;
    }

    SharedState state(fds[0]);
    TestEpollScheduler scheduler;
    auto started = scheduler.start();
    if (!started ||
        !scheduleTask(scheduler, readSequence(&state)) ||
        !scheduleTask(scheduler, simpleWrite(&state)) ||
        !waitUntil([&]() {
            return state.read_suspended.load(std::memory_order_acquire) &&
                   state.write_suspended.load(std::memory_order_acquire);
        })) {
        std::cerr << "[T177] mixed awaitables did not both suspend\n";
        scheduler.stop();
        const bool first_closed = closeSocket(fds[0]);
        const bool second_closed = closeSocket(fds[1]);
        return first_closed && second_closed ? 1 : 2;
    }

    scheduler.stop();
    if (!writeByte(fds[1], 'r') || !drainSocket(fds[1])) {
        std::cerr << "[T177] failed to create simultaneous read/write readiness\n";
        const bool first_closed = closeSocket(fds[0]);
        const bool second_closed = closeSocket(fds[1]);
        return first_closed && second_closed ? 1 : 2;
    }

    const bool polled = scheduler.pollOnceAfterStop();
    const bool sequence_dispatched =
        state.controller.m_sequence_owner[IOController::READ] == nullptr;

    scheduler.finishManualDispatch();
    const bool read_completed = state.read_done.load(std::memory_order_acquire);
    const bool write_completed = state.write_done.load(std::memory_order_acquire);

    scheduler.stop();
    const bool first_closed = closeSocket(fds[0]);
    const bool second_closed = closeSocket(fds[1]);
    if (!polled || !sequence_dispatched || !read_completed || !write_completed ||
        state.read_value.load(std::memory_order_acquire) != 1 ||
        state.write_value.load(std::memory_order_acquire) != 1 ||
        !first_closed || !second_closed) {
        std::cerr << "[T177] one merged poll did not dispatch both mixed owners"
                  << ", polled=" << polled
                  << ", sequence_dispatched=" << sequence_dispatched
                  << ", read_done=" << read_completed
                  << ", write_done=" << write_completed << '\n';
        return 1;
    }

    std::cout << "T177-EpollMixedSequenceWrite PASS\n";
    return 0;
#endif
}
