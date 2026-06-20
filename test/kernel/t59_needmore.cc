/**
 * @file t59_needmore.cc
 * @brief 用途：验证 builder parse 步骤在协议头/包体不完整时不会提前唤醒协程。
 * 关键覆盖点：`ByteQueueView` 累积、`ParseStatus::kNeedMore` 自动重挂上一个读步骤、逻辑完成前协程不恢复。
 * 通过条件：首个半包到达时协程仍未完成，补齐剩余数据后一次成功产出完整帧。
 */

#include <galay/cpp/galay-kernel/core/task.h>
#include <galay/cpp/galay-kernel/core/awaitable.h>
#include <galay/cpp/galay-utils/cache/byte_queue_view.hpp>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <expected>
#include <iostream>
#include <string>
#include <string_view>
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
using galay::utils::ByteQueueView;
using namespace std::chrono_literals;

namespace {

using ParserResult = std::expected<std::string, IOError>;

uint32_t readBigEndian32(const ByteQueueView& queue) {
    auto header = queue.view(0, sizeof(uint32_t));
    return (static_cast<uint32_t>(static_cast<unsigned char>(header[0])) << 24) |
           (static_cast<uint32_t>(static_cast<unsigned char>(header[1])) << 16) |
           (static_cast<uint32_t>(static_cast<unsigned char>(header[2])) << 8) |
           static_cast<uint32_t>(static_cast<unsigned char>(header[3]));
}

std::array<char, 9> makeFrame(std::string_view payload) {
    std::array<char, 9> frame{};
    const uint32_t length = static_cast<uint32_t>(payload.size());
    frame[0] = static_cast<char>((length >> 24) & 0xFF);
    frame[1] = static_cast<char>((length >> 16) & 0xFF);
    frame[2] = static_cast<char>((length >> 8) & 0xFF);
    frame[3] = static_cast<char>(length & 0xFF);
    std::memcpy(frame.data() + 4, payload.data(), payload.size());
    return frame;
}

struct NeedMoreFlow {
    void onRecv(SequenceOps<ParserResult, 4>& ops, RecvIOContext& recv_ctx) {
        ++recv_calls;
        if (!recv_ctx.m_result) {
            ops.complete(std::unexpected(recv_ctx.m_result.error()));
            return;
        }
        inbox.append(scratch, recv_ctx.m_result.value());
    }

    ParseStatus onParse(SequenceOps<ParserResult, 4>& ops) {
        ++parse_calls;
        if (!inbox.has(sizeof(uint32_t))) {
            return ParseStatus::kNeedMore;
        }

        const size_t payload_size = readBigEndian32(inbox);
        if (!inbox.has(sizeof(uint32_t) + payload_size)) {
            return ParseStatus::kNeedMore;
        }

        ops.complete(std::string(inbox.view(sizeof(uint32_t), payload_size)));
        inbox.consume(sizeof(uint32_t) + payload_size);
        return ParseStatus::kCompleted;
    }

    ByteQueueView inbox;
    char scratch[16]{};
    int recv_calls = 0;
    int parse_calls = 0;
};

struct TestState {
    std::atomic<bool> done{false};
    std::atomic<bool> success{false};
    std::atomic<int> recv_calls{0};
    std::atomic<int> parse_calls{0};
};

Task<void> parserTask(TestState* state, int fd) {
    IOController controller(GHandle{.fd = fd});
    NeedMoreFlow flow;

    auto sequence = AwaitableBuilder<ParserResult, 4, NeedMoreFlow>(&controller, flow)
        .recv<&NeedMoreFlow::onRecv>(flow.scratch, sizeof(flow.scratch))
        .parse<&NeedMoreFlow::onParse>()
        .build();

    auto result = co_await sequence;
    state->recv_calls.store(flow.recv_calls, std::memory_order_release);
    state->parse_calls.store(flow.parse_calls, std::memory_order_release);
    state->success.store(result.has_value() && result.value() == "world", std::memory_order_release);
    state->done.store(true, std::memory_order_release);
}

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

}  // namespace

int main() {
    int fds[2] = {-1, -1};
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
        std::cerr << "[T59] socketpair failed: " << std::strerror(errno) << "\n";
        return 1;
    }

    TestScheduler scheduler;
    scheduler.start();

    TestState state;
    scheduleTask(scheduler, parserTask(&state, fds[0]));

    const auto frame = makeFrame("world");
    if (::send(fds[1], frame.data(), 6, 0) != 6) {
        std::cerr << "[T59] failed to send partial frame\n";
        scheduler.stop();
        close(fds[0]);
        close(fds[1]);
        return 1;
    }

    std::this_thread::sleep_for(100ms);
    if (state.done.load(std::memory_order_acquire)) {
        std::cerr << "[T59] coroutine resumed before full frame arrived\n";
        scheduler.stop();
        close(fds[0]);
        close(fds[1]);
        return 1;
    }

    if (::send(fds[1], frame.data() + 6, frame.size() - 6, 0) != static_cast<ssize_t>(frame.size() - 6)) {
        std::cerr << "[T59] failed to send remaining frame\n";
        scheduler.stop();
        close(fds[0]);
        close(fds[1]);
        return 1;
    }

    const bool completed = waitUntil(state.done);
    scheduler.stop();
    close(fds[0]);
    close(fds[1]);

    if (!completed) {
        std::cerr << "[T59] parser coroutine timed out\n";
        return 1;
    }
    if (!state.success.load(std::memory_order_acquire)) {
        std::cerr << "[T59] parser result mismatch\n";
        return 1;
    }
    if (state.recv_calls.load(std::memory_order_acquire) < 2) {
        std::cerr << "[T59] expected at least 2 recv calls, got "
                  << state.recv_calls.load(std::memory_order_acquire) << "\n";
        return 1;
    }
    if (state.parse_calls.load(std::memory_order_acquire) < 2) {
        std::cerr << "[T59] expected at least 2 parse calls, got "
                  << state.parse_calls.load(std::memory_order_acquire) << "\n";
        return 1;
    }

    std::cout << "T59-SequenceParserNeedMore PASS\n";
    return 0;
}
