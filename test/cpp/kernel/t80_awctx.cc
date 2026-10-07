/**
 * @file t80_awctx.cc
 * @brief 用途：验证共享 state-machine awaitable 家族会在挂起前注入 await context。
 * 关键覆盖点：`AwaitContext`、`StateMachineAwaitable::await_suspend()`、
 * `AwaitableBuilder` 链式 flow 对 `on_await_context(...)` 的转发。
 * 通过条件：direct machine / builder flow 两条路径都拿到有效 task 和正确 scheduler。
 */

#include <galay/cpp/galay-kernel/core/awaitable.h>
#include <galay/cpp/galay-kernel/core/task.h>
#include <atomic>
#include <chrono>
#include <expected>
#include <iostream>
#include <optional>
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

using HookResult = std::expected<size_t, IOError>;

struct HookCapture {
    Scheduler* expected_scheduler = nullptr;
    bool bound = false;
    bool scheduler_match = false;
    bool task_valid = false;
};

struct DirectHookMachine {
    using result_type = HookResult;

    explicit DirectHookMachine(HookCapture* capture)
        : m_capture(capture) {}

    void on_await_context(const AwaitContext& ctx) {
        m_capture->bound = true;
        m_capture->scheduler_match = ctx.scheduler == m_capture->expected_scheduler;
        m_capture->task_valid = ctx.task.is_valid();
    }

    MachineAction<result_type> advance() {
        if (m_result.has_value()) {
            return MachineAction<result_type>::complete(std::move(*m_result));
        }
        return MachineAction<result_type>::wait_read(&m_byte, 1);
    }

    void on_read(std::expected<size_t, IOError> result) {
        m_result = std::move(result);
    }

    void on_write(std::expected<size_t, IOError>) {}

    HookCapture* m_capture = nullptr;
    char m_byte = 0;
    std::optional<result_type> m_result;
};

struct BuilderHookFlow {
    explicit BuilderHookFlow(HookCapture* capture)
        : m_capture(capture) {}

    void on_await_context(const AwaitContext& ctx) {
        m_capture->bound = true;
        m_capture->scheduler_match = ctx.scheduler == m_capture->expected_scheduler;
        m_capture->task_valid = ctx.task.is_valid();
    }

    void on_recv(SequenceOps<HookResult, 4>& ops, RecvIOContext& ctx) {
        if (!ctx.m_result) {
            ops.complete(std::unexpected(ctx.m_result.error()));
            return;
        }
        m_bytes = ctx.m_result.value();
    }

    void on_finish(SequenceOps<HookResult, 4>& ops) {
        ops.complete(m_bytes);
    }

    HookCapture* m_capture = nullptr;
    char m_byte = 0;
    size_t m_bytes = 0;
};

struct TestState {
    std::atomic<bool> direct_done{false};
    std::atomic<bool> direct_success{false};
    std::atomic<bool> builder_done{false};
    std::atomic<bool> builder_success{false};
};

Task<void> direct_hook_task(TestState* state, int fd, Scheduler* scheduler) {
    IOController controller(GHandle{.fd = fd});
    HookCapture capture{.expected_scheduler = scheduler};

    auto result = co_await StateMachineAwaitable<DirectHookMachine>(
        &controller,
        DirectHookMachine(&capture));

    const bool success = result.has_value() &&
                         result.value() == 1 &&
                         capture.bound &&
                         capture.scheduler_match &&
                         capture.task_valid;
    state->direct_success.store(success, std::memory_order_release);
    state->direct_done.store(true, std::memory_order_release);
}

Task<void> builder_hook_task(TestState* state, int fd, Scheduler* scheduler) {
    IOController controller(GHandle{.fd = fd});
    HookCapture capture{.expected_scheduler = scheduler};
    BuilderHookFlow flow(&capture);

    auto result = co_await AwaitableBuilder<HookResult, 4, BuilderHookFlow>(&controller, flow)
        .recv<&BuilderHookFlow::on_recv>(&flow.m_byte, 1)
        .finish<&BuilderHookFlow::on_finish>()
        .build();

    const bool success = result.has_value() &&
                         result.value() == 1 &&
                         capture.bound &&
                         capture.scheduler_match &&
                         capture.task_valid;
    state->builder_success.store(success, std::memory_order_release);
    state->builder_done.store(true, std::memory_order_release);
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
    int direct_fds[2] = {-1, -1};
    int builder_fds[2] = {-1, -1};
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, direct_fds) != 0) {
        std::perror("[T80] direct socketpair");
        return 1;
    }
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, builder_fds) != 0) {
        std::perror("[T80] builder socketpair");
        close(direct_fds[0]);
        close(direct_fds[1]);
        return 1;
    }

    TestScheduler scheduler;
    scheduler.start();

    TestState state;
    schedule_task(scheduler, direct_hook_task(&state, direct_fds[0], &scheduler));
    schedule_task(scheduler, builder_hook_task(&state, builder_fds[0], &scheduler));

    constexpr char kPayload = 'x';
    ::send(direct_fds[1], &kPayload, 1, 0);
    ::send(builder_fds[1], &kPayload, 1, 0);

    const bool direct_completed = wait_until(state.direct_done);
    const bool builder_completed = wait_until(state.builder_done);

    scheduler.stop();
    close(direct_fds[0]);
    close(direct_fds[1]);
    close(builder_fds[0]);
    close(builder_fds[1]);

    if (!direct_completed || !state.direct_success.load(std::memory_order_acquire)) {
        std::cerr << "[T80] direct state-machine await context hook failed\n";
        return 1;
    }
    if (!builder_completed || !state.builder_success.load(std::memory_order_acquire)) {
        std::cerr << "[T80] builder flow await context hook failed\n";
        return 1;
    }

    std::cout << "T80-StateMachineAwaitContext PASS\n";
    return 0;
}
