/**
 * @file t35_nullprobe.cc
 * @brief 用途：验证 io_uring 组合式 SequenceAwaitable 不依赖空指针探测也能完成注册。
 * 关键覆盖点：sequence 路径注册、io_uring 路径接线、无空探测完成恢复。
 * 通过条件：io_uring sequence 路径可用且测试返回 0。
 */

#include "galay-kernel/core/task.h"
#include "galay-kernel/core/awaitable.h"
#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>

#ifdef USE_IOURING
#include "galay-kernel/core/uring_scheduler.h"
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

using namespace galay::kernel;
using namespace std::chrono_literals;

namespace {

struct ProbeSendContext : public SendIOContext {
    using SendIOContext::SendIOContext;

    bool handleComplete(struct io_uring_cqe* cqe, GHandle handle) override {
        if (cqe == nullptr) {
            ++null_probes;
            return false;
        }
        return SendIOContext::handleComplete(cqe, handle);
    }

    int null_probes = 0;
};

using ProbeResult = std::expected<size_t, IOError>;

struct ProbeFlow {
    void onSend(SequenceOps<ProbeResult, 2>& ops, ProbeSendContext& send_ctx);

    using SendStep = SequenceStep<ProbeResult, 2, ProbeFlow, ProbeSendContext, &ProbeFlow::onSend>;

    ProbeFlow(const char* buffer, size_t length)
        : send(this, buffer, length) {}

    auto make(IOController* controller) -> SequenceAwaitable<ProbeResult, 2> {
        SequenceAwaitable<ProbeResult, 2> awaitable(controller);
        awaitable.queue(send);
        return awaitable;
    }

    SendStep send;
    int null_probes = 0;
};

inline void ProbeFlow::onSend(SequenceOps<ProbeResult, 2>& ops, ProbeSendContext& send_ctx) {
    null_probes = send_ctx.null_probes;
    ops.complete(std::move(send_ctx.m_result));
}

struct TestState {
    std::atomic<bool> done{false};
    std::atomic<bool> success{false};
    std::atomic<int> null_probes{0};
};

Task<void> sendTask(TestState* state, int fd, const char* msg, size_t len) {
    IOController controller(GHandle{.fd = fd});
    ProbeFlow flow(msg, len);
    auto sequence = flow.make(&controller);
    auto result = co_await sequence;

    state->null_probes.store(flow.null_probes, std::memory_order_release);
    state->success.store(result.has_value() && result.value() == len, std::memory_order_release);
    state->done.store(true, std::memory_order_release);
    co_return;
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
        std::cerr << "[T35] socketpair failed: " << std::strerror(errno) << "\n";
        return 1;
    }

    for (int fd : fds) {
        const int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }

    const char payload[] = "hello-iouring-sequence";
    char recv_buf[64]{};
    std::atomic<bool> peer_done{false};
    std::thread peer([&]() {
        const auto deadline = std::chrono::steady_clock::now() + 1s;
        while (std::chrono::steady_clock::now() < deadline) {
            const ssize_t n = recv(fds[1], recv_buf, sizeof(recv_buf), 0);
            if (n == static_cast<ssize_t>(sizeof(payload) - 1)) {
                peer_done.store(true, std::memory_order_release);
                return;
            }
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
                std::this_thread::sleep_for(2ms);
                continue;
            }
            break;
        }
        peer_done.store(false, std::memory_order_release);
    });

    TestState state;
    IOUringScheduler scheduler;
    scheduler.start();
    scheduleTask(scheduler, sendTask(&state, fds[0], payload, sizeof(payload) - 1));

    const bool coroutine_done = waitUntil(state.done);
    scheduler.stop();
    peer.join();

    close(fds[0]);
    close(fds[1]);

    if (!coroutine_done) {
        std::cerr << "[T35] coroutine timed out\n";
        return 1;
    }
    if (!state.success.load(std::memory_order_acquire)) {
        std::cerr << "[T35] send coroutine failed\n";
        return 1;
    }
    if (!peer_done.load(std::memory_order_acquire)) {
        std::cerr << "[T35] peer recv failed\n";
        return 1;
    }
    if (std::strcmp(recv_buf, payload) != 0) {
        std::cerr << "[T35] payload mismatch: " << recv_buf << "\n";
        return 1;
    }
    if (state.null_probes.load(std::memory_order_acquire) != 0) {
        std::cerr << "[T35] expected no null cqe probes, got "
                  << state.null_probes.load(std::memory_order_acquire) << "\n";
        return 1;
    }

    std::cout << "T35-IOUringSequenceNoNullProbe PASS\n";
    return 0;
}

#else

int main() {
    std::cout << "T35-IOUringSequenceNoNullProbe SKIP\n";
    return 0;
}

#endif
