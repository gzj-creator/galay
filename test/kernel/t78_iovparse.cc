/**
 * @file t78_iovparse.cc
 * @brief 用途：验证 builder `readv -> parse -> writev` 流程能在半包场景下重臂 readv。
 * 关键覆盖点：`AwaitableBuilder::readv(...)`、`parse<&Flow::onParse>()`、
 * `AwaitableBuilder::writev(...)` 的混合桥接，以及 `ParseStatus::kNeedMore`
 * 在 iovec 接收路径上的重入行为。
 * 通过条件：读到分段 frame 后解析出 payload，并回写 `ACK:` + payload。
 */

#include <galay/cpp/galay-kernel/core/awaitable.h>
#include <galay/cpp/galay-kernel/core/task.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
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

using BridgeResult = std::expected<std::string, IOError>;

struct TestState {
    std::atomic<bool> done{false};
    std::atomic<bool> success{false};
    std::atomic<bool> first_fragment_sent{false};
    std::atomic<bool> allow_second_fragment{false};
    std::atomic<bool> abort_peer{false};
    std::atomic<bool> ack_ok{false};
    std::atomic<bool> writev_ok{false};
    std::atomic<uint32_t> readv_count{0};
    std::atomic<uint32_t> need_more_count{0};
};

struct ParseBridgeFlow {
    explicit ParseBridgeFlow(TestState* state_ptr)
        : state(state_ptr)
    {
        recv_iovecs[0].iov_base = header_scratch.data();
        recv_iovecs[0].iov_len = header_scratch.size();
        recv_iovecs[1].iov_base = body_scratch.data();
        recv_iovecs[1].iov_len = body_scratch.size();
        send_iovecs[0].iov_base = ack_prefix.data();
        send_iovecs[0].iov_len = ack_prefix.size();
        send_iovecs[1].iov_base = nullptr;
        send_iovecs[1].iov_len = 0;
    }

    void onReadv(SequenceOps<BridgeResult, 4>& ops, ReadvIOContext& ctx) {
        if (!ctx.m_result) {
            ops.complete(std::unexpected(ctx.m_result.error()));
            return;
        }

        state->readv_count.fetch_add(1, std::memory_order_release);
        const size_t total = ctx.m_result.value();
        const size_t head_bytes = std::min(total, header_scratch.size());
        const size_t body_bytes = total > header_scratch.size() ? total - header_scratch.size() : 0;
        inbox.append(header_scratch.data(), head_bytes);
        inbox.append(body_scratch.data(), body_bytes);
    }

    ParseStatus onParse(SequenceOps<BridgeResult, 4>&) {
        if (inbox.size() < sizeof(uint32_t)) {
            state->need_more_count.fetch_add(1, std::memory_order_release);
            return ParseStatus::kNeedMore;
        }

        const uint32_t payload_length =
            (static_cast<uint8_t>(inbox[0]) << 24) |
            (static_cast<uint8_t>(inbox[1]) << 16) |
            (static_cast<uint8_t>(inbox[2]) << 8) |
            static_cast<uint8_t>(inbox[3]);

        if (inbox.size() < sizeof(uint32_t) + payload_length) {
            state->need_more_count.fetch_add(1, std::memory_order_release);
            return ParseStatus::kNeedMore;
        }

        payload = inbox.substr(sizeof(uint32_t), payload_length);
        send_iovecs[1].iov_base = payload.data();
        send_iovecs[1].iov_len = payload.size();
        return ParseStatus::kCompleted;
    }

    void onWritev(SequenceOps<BridgeResult, 4>& ops, WritevIOContext& ctx) {
        if (!ctx.m_result) {
            ops.complete(std::unexpected(ctx.m_result.error()));
            return;
        }
        state->writev_ok.store(
            ctx.m_result.value() == ack_prefix.size() + payload.size(),
            std::memory_order_release
        );
        ops.complete(payload);
    }

    std::array<char, 4> header_scratch{};
    std::array<char, 16> body_scratch{};
    std::array<struct iovec, 2> recv_iovecs{};
    std::array<struct iovec, 2> send_iovecs{};
    std::array<char, 4> ack_prefix{'A', 'C', 'K', ':'};
    std::string inbox;
    std::string payload;
    TestState* state;
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

bool waitUntilAtLeast(const std::atomic<uint32_t>& value,
                      uint32_t target,
                      std::chrono::milliseconds timeout = 1000ms,
                      std::chrono::milliseconds step = 2ms) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (value.load(std::memory_order_acquire) >= target) {
            return true;
        }
        std::this_thread::sleep_for(step);
    }
    return value.load(std::memory_order_acquire) >= target;
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

bool setNonBlocking(int fd) {
    const int flags = fcntl(fd, F_GETFL, 0);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

template <typename BuilderT>
auto makeParseBridgeAwaitable(BuilderT& builder, ParseBridgeFlow& flow) {
    auto with_readv = builder.template readv<&ParseBridgeFlow::onReadv>(
        flow.recv_iovecs,
        flow.recv_iovecs.size()
    );
    auto with_parse = with_readv.template parse<&ParseBridgeFlow::onParse>();
    auto with_writev = with_parse.template writev<&ParseBridgeFlow::onWritev>(
        flow.send_iovecs,
        flow.send_iovecs.size()
    );
    return with_writev.build();
}

Task<void> parseBridgeTask(TestState* state, int fd) {
    IOController controller(GHandle{.fd = fd});
    ParseBridgeFlow flow(state);

    auto builder = AwaitableBuilder<BridgeResult, 4, ParseBridgeFlow>(&controller, flow);
    auto awaitable = makeParseBridgeAwaitable(builder, flow);

    auto result = co_await awaitable;
    state->success.store(
        result.has_value() &&
            result.value() == "ping" &&
            state->writev_ok.load(std::memory_order_acquire) &&
            state->need_more_count.load(std::memory_order_acquire) >= 1 &&
            state->readv_count.load(std::memory_order_acquire) >= 2,
        std::memory_order_release
    );
    state->done.store(true, std::memory_order_release);
}

}  // namespace

int main() {
    int fds[2] = {-1, -1};
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
        std::cerr << "[T78] socketpair failed: " << std::strerror(errno) << "\n";
        return 1;
    }
    if (!setNonBlocking(fds[0])) {
        std::cerr << "[T78] failed to set scheduler fd non-blocking\n";
        close(fds[0]);
        close(fds[1]);
        return 1;
    }

    TestScheduler scheduler;
    scheduler.start();

    TestState state;
    std::thread peer([&]() {
        const uint8_t frame[] = {
            0x00, 0x00, 0x00, 0x04,
            'p', 'i', 'n', 'g',
        };
        char ack[8]{};

        sendAll(fds[1], reinterpret_cast<const char*>(frame), 2);
        state.first_fragment_sent.store(true, std::memory_order_release);
        while (!state.allow_second_fragment.load(std::memory_order_acquire) &&
               !state.abort_peer.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(1ms);
        }
        if (state.abort_peer.load(std::memory_order_acquire)) {
            return;
        }
        if (sendAll(fds[1], reinterpret_cast<const char*>(frame + 2), sizeof(frame) - 2) &&
            recvExact(fds[1], ack, sizeof(ack))) {
            state.ack_ok.store(std::string(ack, sizeof(ack)) == "ACK:ping", std::memory_order_release);
        }
    });

    scheduleTask(scheduler, parseBridgeTask(&state, fds[0]));

    const bool first_fragment_observed = waitUntil(state.first_fragment_sent, 500ms);
    if (!first_fragment_observed) {
        state.abort_peer.store(true, std::memory_order_release);
        state.allow_second_fragment.store(true, std::memory_order_release);
        scheduler.stop();
        close(fds[0]);
        peer.join();
        close(fds[1]);
        std::cerr << "[T78] first fragment was not observed\n";
        return 1;
    }

    const bool first_read_seen = waitUntilAtLeast(state.readv_count, 1, 500ms);
    const bool need_more_seen = waitUntilAtLeast(state.need_more_count, 1, 500ms);
    if (!first_read_seen || !need_more_seen || state.done.load(std::memory_order_acquire)) {
        state.abort_peer.store(true, std::memory_order_release);
        state.allow_second_fragment.store(true, std::memory_order_release);
        scheduler.stop();
        close(fds[0]);
        peer.join();
        close(fds[1]);
        std::cerr << "[T78] builder iovec parse bridge did not suspend on the first fragment\n";
        return 1;
    }
    state.allow_second_fragment.store(true, std::memory_order_release);

    const bool completed = waitUntil(state.done);
    scheduler.stop();
    close(fds[0]);
    peer.join();
    close(fds[1]);

    if (!completed) {
        std::cerr << "[T78] builder iovec parse bridge timed out\n";
        return 1;
    }
    if (!state.success.load(std::memory_order_acquire) ||
        !state.ack_ok.load(std::memory_order_acquire)) {
        std::cerr << "[T78] builder iovec parse bridge mismatch\n";
        return 1;
    }

    std::cout << "T78-AwaitableBuilderIovecParseBridge PASS\n";
    return 0;
}
