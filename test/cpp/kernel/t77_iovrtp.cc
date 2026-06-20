/**
 * @file t77_iovrtp.cc
 * @brief 用途：验证 builder `writev` 与 `readv` 能完成双端 scatter-gather 往返。
 * 关键覆盖点：链式 `AwaitableBuilder::writev(...)`、`AwaitableBuilder::readv(...)`、
 * builder 驱动的字节数统计与载荷一致性。
 * 通过条件：writer 写出 `HEAD:body-data`，reader 分段收到完全一致的内容。
 */

#include <galay/cpp/galay-kernel/core/awaitable.h>
#include <galay/cpp/galay-kernel/core/task.h>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <expected>
#include <fcntl.h>
#include <iostream>
#include <string>
#include <sys/socket.h>
#include <sys/uio.h>
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

using IovecResult = std::expected<size_t, IOError>;

struct WriteFlow {
    void onWritev(SequenceOps<IovecResult, 4>& ops, WritevIOContext& ctx) {
        ops.complete(std::move(ctx.m_result));
    }
};

struct ReadFlow {
    void onReadv(SequenceOps<IovecResult, 4>& ops, ReadvIOContext& ctx) {
        ops.complete(std::move(ctx.m_result));
    }
};

struct TestState {
    std::atomic<bool> writer_done{false};
    std::atomic<bool> reader_done{false};
    std::atomic<bool> writer_ok{false};
    std::atomic<bool> reader_ok{false};
};

template <typename BuilderT>
auto makeWriteAwaitable(BuilderT& builder, std::array<struct iovec, 2>& iovecs) {
    auto with_writev = builder.template writev<&WriteFlow::onWritev>(iovecs, iovecs.size());
    return with_writev.build();
}

template <typename BuilderT>
auto makeReadAwaitable(BuilderT& builder, std::array<struct iovec, 2>& iovecs) {
    auto with_readv = builder.template readv<&ReadFlow::onReadv>(iovecs, iovecs.size());
    return with_readv.build();
}

bool setNonBlocking(int fd) {
    const int flags = fcntl(fd, F_GETFL, 0);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

bool waitUntilBoth(const std::atomic<bool>& first,
                   const std::atomic<bool>& second,
                   std::chrono::milliseconds timeout = 1000ms,
                   std::chrono::milliseconds step = 2ms) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (first.load(std::memory_order_acquire) && second.load(std::memory_order_acquire)) {
            return true;
        }
        std::this_thread::sleep_for(step);
    }
    return first.load(std::memory_order_acquire) && second.load(std::memory_order_acquire);
}

Task<void> writerTask(TestState* state, int fd) {
    IOController controller(GHandle{.fd = fd});
    WriteFlow flow;

    constexpr char kHead[] = "HEAD:";
    constexpr char kBody[] = "body-data";
    std::array<struct iovec, 2> iovecs{};
    iovecs[0].iov_base = const_cast<char*>(kHead);
    iovecs[0].iov_len = sizeof(kHead) - 1;
    iovecs[1].iov_base = const_cast<char*>(kBody);
    iovecs[1].iov_len = sizeof(kBody) - 1;

    auto builder = AwaitableBuilder<IovecResult, 4, WriteFlow>(&controller, flow);
    auto awaitable = makeWriteAwaitable(builder, iovecs);

    auto result = co_await awaitable;
    state->writer_ok.store(
        result.has_value() && result.value() == (sizeof(kHead) - 1) + (sizeof(kBody) - 1),
        std::memory_order_release
    );
    state->writer_done.store(true, std::memory_order_release);
}

Task<void> readerTask(TestState* state, int fd) {
    IOController controller(GHandle{.fd = fd});
    ReadFlow flow;

    std::array<char, 5> head{};
    std::array<char, 9> body{};
    std::array<struct iovec, 2> iovecs{};
    iovecs[0].iov_base = head.data();
    iovecs[0].iov_len = head.size();
    iovecs[1].iov_base = body.data();
    iovecs[1].iov_len = body.size();

    auto builder = AwaitableBuilder<IovecResult, 4, ReadFlow>(&controller, flow);
    auto awaitable = makeReadAwaitable(builder, iovecs);

    auto result = co_await awaitable;
    state->reader_ok.store(
        result.has_value() &&
            result.value() == head.size() + body.size() &&
            std::string(head.data(), head.size()) == "HEAD:" &&
            std::string(body.data(), body.size()) == "body-data",
        std::memory_order_release
    );
    state->reader_done.store(true, std::memory_order_release);
}

}  // namespace

int main() {
    int fds[2] = {-1, -1};
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
        std::cerr << "[T77] socketpair failed: " << std::strerror(errno) << "\n";
        return 1;
    }
    if (!setNonBlocking(fds[0]) || !setNonBlocking(fds[1])) {
        std::cerr << "[T77] failed to set socketpair non-blocking\n";
        close(fds[0]);
        close(fds[1]);
        return 1;
    }

    TestScheduler scheduler;
    scheduler.start();

    TestState state;
    scheduleTask(scheduler, readerTask(&state, fds[1]));
    scheduleTask(scheduler, writerTask(&state, fds[0]));

    const bool completed = waitUntilBoth(state.writer_done, state.reader_done);
    scheduler.stop();
    close(fds[0]);
    close(fds[1]);

    if (!completed) {
        std::cerr << "[T77] builder iovec roundtrip timed out\n";
        return 1;
    }
    if (!state.writer_ok.load(std::memory_order_acquire) ||
        !state.reader_ok.load(std::memory_order_acquire)) {
        std::cerr << "[T77] builder iovec roundtrip mismatch\n";
        return 1;
    }

    std::cout << "T77-AwaitableBuilderIovecRoundtrip PASS\n";
    return 0;
}
