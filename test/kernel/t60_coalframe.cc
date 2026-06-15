/**
 * @file t60_coalframe.cc
 * @brief 用途：验证 builder parse 步骤在单次 recv 收到粘包时能在一次逻辑恢复内继续本地解析。
 * 关键覆盖点：`ParseStatus::kContinue`、单次 recv 后的连续 parse loop、一次恢复吃完多个协议帧。
 * 通过条件：单次 recv 解析出两帧，且无需第二次底层 recv。
 */

#include "galay-kernel/core/task.h"
#include "galay-kernel/core/awaitable.h"
#include <galay-utils/cache/byte_queue_view.hpp>
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
#include <vector>

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
using galay::utils::ByteQueueView;
using namespace std::chrono_literals;

namespace {

using ParserResult = std::expected<std::vector<std::string>, IOError>;

uint32_t readBigEndian32(const ByteQueueView& queue) {
    auto header = queue.view(0, sizeof(uint32_t));
    return (static_cast<uint32_t>(static_cast<unsigned char>(header[0])) << 24) |
           (static_cast<uint32_t>(static_cast<unsigned char>(header[1])) << 16) |
           (static_cast<uint32_t>(static_cast<unsigned char>(header[2])) << 8) |
           static_cast<uint32_t>(static_cast<unsigned char>(header[3]));
}

std::array<char, 18> makeTwoFrames() {
    std::array<char, 18> buffer{};
    const auto frame_a = std::array<char, 9>{0, 0, 0, 5, 'a', 'l', 'p', 'h', 'a'};
    const auto frame_b = std::array<char, 9>{0, 0, 0, 5, 'b', 'r', 'a', 'v', 'o'};
    std::memcpy(buffer.data(), frame_a.data(), frame_a.size());
    std::memcpy(buffer.data() + frame_a.size(), frame_b.data(), frame_b.size());
    return buffer;
}

struct CoalescedFlow {
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

        frames.emplace_back(inbox.view(sizeof(uint32_t), payload_size));
        inbox.consume(sizeof(uint32_t) + payload_size);
        if (frames.size() == 2) {
            ops.complete(frames);
            return ParseStatus::kCompleted;
        }
        return ParseStatus::kContinue;
    }

    ByteQueueView inbox;
    char scratch[32]{};
    std::vector<std::string> frames;
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
    CoalescedFlow flow;

    auto sequence = AwaitableBuilder<ParserResult, 4, CoalescedFlow>(&controller, flow)
        .recv<&CoalescedFlow::onRecv>(flow.scratch, sizeof(flow.scratch))
        .parse<&CoalescedFlow::onParse>()
        .build();

    auto result = co_await sequence;
    state->recv_calls.store(flow.recv_calls, std::memory_order_release);
    state->parse_calls.store(flow.parse_calls, std::memory_order_release);
    state->success.store(result.has_value() &&
                             result->size() == 2 &&
                             result->at(0) == "alpha" &&
                             result->at(1) == "bravo",
                         std::memory_order_release);
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
        std::cerr << "[T60] socketpair failed: " << std::strerror(errno) << "\n";
        return 1;
    }

    TestScheduler scheduler;
    scheduler.start();

    TestState state;
    scheduleTask(scheduler, parserTask(&state, fds[0]));

    const auto payload = makeTwoFrames();
    if (::send(fds[1], payload.data(), payload.size(), 0) != static_cast<ssize_t>(payload.size())) {
        std::cerr << "[T60] failed to send coalesced frames\n";
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
        std::cerr << "[T60] parser coroutine timed out\n";
        return 1;
    }
    if (!state.success.load(std::memory_order_acquire)) {
        std::cerr << "[T60] parser result mismatch\n";
        return 1;
    }
    if (state.recv_calls.load(std::memory_order_acquire) != 1) {
        std::cerr << "[T60] expected exactly 1 recv call, got "
                  << state.recv_calls.load(std::memory_order_acquire) << "\n";
        return 1;
    }
    if (state.parse_calls.load(std::memory_order_acquire) < 2) {
        std::cerr << "[T60] expected at least 2 parse calls, got "
                  << state.parse_calls.load(std::memory_order_acquire) << "\n";
        return 1;
    }

    std::cout << "T60-SequenceParserCoalescedFrames PASS\n";
    return 0;
}
