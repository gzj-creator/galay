/**
 * @file e11_proto.cc
 * @brief 用途：用模块导入方式演示链式 AwaitableBuilder 的最小协议解析流程。
 * 关键覆盖点：`recv -> parse -> send`、`ByteQueueView`、`ParseStatus::kNeedMore`。
 * 通过条件：长度前缀 `ping` 帧拆包送达后，builder 成功回写 `pong` 并返回 0。
 */

import galay.kernel;

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <expected>
#include <iostream>
#include <string>
#include <sys/socket.h>
#include <sys/time.h>
#include <thread>
#include <unistd.h>

using namespace galay::kernel;
using namespace std::chrono_literals;

namespace {

using ExampleResult = std::expected<std::string, IOError>;

uint32_t readBigEndian32(const ByteQueueView& queue) {
    auto header = queue.view(0, sizeof(uint32_t));
    return (static_cast<uint32_t>(static_cast<unsigned char>(header[0])) << 24) |
           (static_cast<uint32_t>(static_cast<unsigned char>(header[1])) << 16) |
           (static_cast<uint32_t>(static_cast<unsigned char>(header[2])) << 8) |
           static_cast<uint32_t>(static_cast<unsigned char>(header[3]));
}

std::array<char, 8> makeFrame() {
    std::array<char, 8> frame{};
    constexpr uint32_t kLength = 4;
    frame[0] = static_cast<char>((kLength >> 24) & 0xFF);
    frame[1] = static_cast<char>((kLength >> 16) & 0xFF);
    frame[2] = static_cast<char>((kLength >> 8) & 0xFF);
    frame[3] = static_cast<char>(kLength & 0xFF);
    std::memcpy(frame.data() + 4, "ping", 4);
    return frame;
}

struct BuilderFlow {
    void onRecv(SequenceOps<ExampleResult, 4>& ops, RecvIOContext& recv_ctx) {
        if (!recv_ctx.m_result) {
            ops.complete(std::unexpected(recv_ctx.m_result.error()));
            return;
        }
        inbox.append(scratch, recv_ctx.m_result.value());
    }

    ParseStatus onParse(SequenceOps<ExampleResult, 4>& ops) {
        if (!inbox.has(sizeof(uint32_t))) {
            return ParseStatus::kNeedMore;
        }

        const size_t payload_size = readBigEndian32(inbox);
        if (!inbox.has(sizeof(uint32_t) + payload_size)) {
            return ParseStatus::kNeedMore;
        }

        const std::string payload(inbox.view(sizeof(uint32_t), payload_size));
        inbox.consume(sizeof(uint32_t) + payload_size);

        if (payload != "ping") {
            ops.complete(std::unexpected(IOError(kParamInvalid, 0)));
            return ParseStatus::kCompleted;
        }

        parsed_ping = true;
        return ParseStatus::kCompleted;
    }

    void onSend(SequenceOps<ExampleResult, 4>& ops, SendIOContext& send_ctx) {
        if (!send_ctx.m_result) {
            ops.complete(std::unexpected(send_ctx.m_result.error()));
            return;
        }
        if (!parsed_ping || send_ctx.m_result.value() != reply.size()) {
            ops.complete(std::unexpected(IOError(kSendFailed, 0)));
            return;
        }
        ops.complete(std::string(reply.data(), reply.size()));
    }

    ByteQueueView inbox;
    char scratch[16]{};
    std::array<char, 4> reply{'p', 'o', 'n', 'g'};
    bool parsed_ping = false;
};

struct ExampleState {
    std::atomic<bool> done{false};
    std::atomic<bool> builder_ok{false};
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

Task<void> builderProtocolTask(ExampleState* state, int fd) {
    IOController controller(GHandle{.fd = fd});
    BuilderFlow flow;
    auto awaitable = AwaitableBuilder<ExampleResult, 4, BuilderFlow>(&controller, flow)
        .recv<&BuilderFlow::onRecv>(flow.scratch, sizeof(flow.scratch))
        .parse<&BuilderFlow::onParse>()
        .send<&BuilderFlow::onSend>(flow.reply.data(), flow.reply.size())
        .build();

    auto result = co_await awaitable;
    state->builder_ok.store(
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
        const auto frame = makeFrame();
        char pong[4]{};

        if (!sendAll(fds[1], frame.data(), 6)) {
            return;
        }
        std::this_thread::sleep_for(50ms);
        if (state.done.load(std::memory_order_acquire)) {
            return;
        }
        if (!sendAll(fds[1], frame.data() + 6, frame.size() - 6)) {
            return;
        }
        if (recvExact(fds[1], pong, sizeof(pong)) &&
            std::string(pong, sizeof(pong)) == "pong") {
            state.peer_ok.store(true, std::memory_order_release);
        }
    });

    Runtime runtime = RuntimeBuilder()
        .ioSchedulerCount(1)
        .computeSchedulerCount(1)
        .build();
    runtime.start();

    scheduleTask(runtime.getNextIOScheduler(), builderProtocolTask(&state, fds[0]));

    const bool completed = waitUntil(state.done, 2000ms);

    runtime.stop();
    close(fds[0]);
    close(fds[1]);
    peer.join();

    if (!completed) {
        std::cerr << "builder protocol import example timed out\n";
        return 1;
    }
    if (!state.builder_ok.load(std::memory_order_acquire) ||
        !state.peer_ok.load(std::memory_order_acquire)) {
        std::cerr << "builder protocol import example failed\n";
        return 1;
    }

    std::cout << "builder protocol import example passed\n";
    return 0;
}
