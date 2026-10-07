/**
 * @file t70_smbridge.cc
 * @brief 用途：验证 builder 的状态机入口与链式 build 桥接都能落到共享状态机内核。
 * 关键覆盖点：`AwaitableBuilder<ResultT>::from_state_machine(...).build()`、
 * 链式 `recv.parse.send.build()` 不再直接返回旧 `SequenceAwaitable` 路径。
 * 通过条件：状态机入口运行成功，且链式 build 的类型桥接静态断言成立。
 */

#include <galay/cpp/galay-kernel/core/awaitable.h>
#include <galay/cpp/galay-kernel/core/task.h>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <expected>
#include <iostream>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <type_traits>
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

using BuilderResult = std::expected<std::string, IOError>;

struct ReadOnceMachine {
    using result_type = BuilderResult;

    MachineAction<result_type> advance() {
        if (m_result.has_value()) {
            return MachineAction<result_type>::complete(std::move(*m_result));
        }
        return MachineAction<result_type>::wait_read(m_buffer, sizeof(m_buffer));
    }

    void on_read(std::expected<size_t, IOError> result) {
        if (!result) {
            m_result = std::unexpected(result.error());
            return;
        }
        m_result = std::string(m_buffer, result.value());
    }

    void on_write(std::expected<size_t, IOError>) {}

private:
    char m_buffer[8]{};
    std::optional<BuilderResult> m_result;
};

struct ChainSurfaceFlow {
    std::array<char, 8> scratch{};
    std::array<char, 4> reply{'p', 'o', 'n', 'g'};

    void on_recv(SequenceOps<BuilderResult, 4>&, RecvIOContext&) {}

    ParseStatus on_parse(SequenceOps<BuilderResult, 4>&) {
        return ParseStatus::kCompleted;
    }

    void on_send(SequenceOps<BuilderResult, 4>& ops, SendIOContext&) {
        ops.complete(std::string("pong"));
    }
};

using ChainedAwaitableT = decltype(
    std::declval<AwaitableBuilder<BuilderResult, 4, ChainSurfaceFlow>&>()
        .template recv<&ChainSurfaceFlow::on_recv>(std::declval<char*>(), std::declval<size_t>())
        .template parse<&ChainSurfaceFlow::on_parse>()
        .template send<&ChainSurfaceFlow::on_send>(std::declval<const char*>(), std::declval<size_t>())
        .build()
);

static_assert(
    !std::derived_from<std::remove_cvref_t<ChainedAwaitableT>, SequenceAwaitable<BuilderResult, 4>>,
    "Chained AwaitableBuilder::build() should bridge to the shared state-machine core"
);

struct TestState {
    std::atomic<bool> done{false};
    std::atomic<bool> success{false};
};

Task<void> builder_task(TestState* state, int fd) {
    IOController controller(GHandle{.fd = fd});
    auto awaitable = AwaitableBuilder<BuilderResult>::from_state_machine(
        &controller,
        ReadOnceMachine{}
    ).build();

    auto result = co_await awaitable;
    state->success.store(result.has_value() && result.value() == "hello", std::memory_order_release);
    state->done.store(true, std::memory_order_release);
}

[[maybe_unused]] Task<void> chained_builder_surface_task(int fd) {
    IOController controller(GHandle{.fd = fd});
    ChainSurfaceFlow flow;
    auto awaitable = AwaitableBuilder<BuilderResult, 4, ChainSurfaceFlow>(&controller, flow)
        .recv<&ChainSurfaceFlow::on_recv>(flow.scratch.data(), flow.scratch.size())
        .parse<&ChainSurfaceFlow::on_parse>()
        .send<&ChainSurfaceFlow::on_send>(flow.reply.data(), flow.reply.size())
        .build();

    auto result = co_await awaitable;
    (void)result;
}

bool wait_until(const std::atomic<bool>& flag,
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
        std::cerr << "[T70] socketpair failed: " << std::strerror(errno) << "\n";
        return 1;
    }

    TestScheduler scheduler;
    scheduler.start();

    TestState state;
    schedule_task(scheduler, builder_task(&state, fds[0]));
    constexpr char payload[] = "hello";
    ::send(fds[1], payload, sizeof(payload) - 1, 0);

    const bool completed = wait_until(state.done);
    scheduler.stop();
    close(fds[0]);
    close(fds[1]);

    if (!completed) {
        std::cerr << "[T70] builder state machine timed out\n";
        return 1;
    }
    if (!state.success.load(std::memory_order_acquire)) {
        std::cerr << "[T70] builder state machine result mismatch\n";
        return 1;
    }

    std::cout << "T70-AwaitableBuilderStateMachineBridge PASS\n";
    return 0;
}
