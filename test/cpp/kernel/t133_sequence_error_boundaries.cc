/**
 * @file t133_sequence_error_boundaries.cc
 * @brief 验证 sequence awaitable 的错误路径通过 std::expected 返回而不是终止进程。
 *
 * 关键覆盖点：
 * - `SequenceAwaitable::queue(...)` 超过内联容量时返回 `IOError(kParamInvalid)`。
 * - kernel awaitable 公共错误路径不再包含 `std::abort()`。
 */

#include <galay/cpp/galay-kernel/core/awaitable.h>
#include <galay/cpp/galay-kernel/core/task.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <expected>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <sys/wait.h>
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

using BoundaryResult = std::expected<int, IOError>;
enum class BoundaryDomainError : int {
    kNone = 0,
    kReadClaimRejected = 1,
};
using BoundaryDomainResult = std::expected<int, BoundaryDomainError>;

struct BoundaryFlow {
    void on_local(SequenceOps<BoundaryResult, 1>&) {}
};

using BoundaryStep = LocalSequenceStep<
    BoundaryResult,
    1,
    BoundaryFlow,
    &BoundaryFlow::on_local>;

struct DomainClaimFailureMachine {
    using result_type = BoundaryDomainResult;
    static constexpr auto kSequenceOwnerDomain = SequenceOwnerDomain::Read;

    MachineAction<result_type> advance() {
        if (m_result.has_value()) {
            return MachineAction<result_type>::complete(std::move(*m_result));
        }
        return MachineAction<result_type>::wait_read(&m_byte, 1);
    }

    void on_read(std::expected<size_t, IOError> result) {
        if (!result) {
            m_result = std::unexpected(BoundaryDomainError::kReadClaimRejected);
            return;
        }
        m_result = static_cast<int>(result.value());
    }

    void on_write(std::expected<size_t, IOError>) {}

    char m_byte = 0;
    std::optional<result_type> m_result;
};

struct DomainClaimState {
    std::atomic<bool> done{false};
    std::atomic<int> error{0};
};

Task<void> domain_claim_failure_task(DomainClaimState* state)
{
    IOController controller(GHandle::invalid());
    controller.m_sequence_owner[IOController::READ] =
        reinterpret_cast<SequenceAwaitableBase*>(static_cast<uintptr_t>(1));

    auto result = co_await StateMachineAwaitable<DomainClaimFailureMachine>(
        &controller, DomainClaimFailureMachine{});
    state->error.store(result ? 0 : static_cast<int>(result.error()), std::memory_order_release);
    state->done.store(true, std::memory_order_release);
}

bool wait_until_done(const std::atomic<bool>& flag,
                   std::chrono::milliseconds timeout = 1000ms,
                   std::chrono::milliseconds step = 2ms)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (flag.load(std::memory_order_acquire)) {
            return true;
        }
        std::this_thread::sleep_for(step);
    }
    return flag.load(std::memory_order_acquire);
}

std::string read_all(const std::filesystem::path& path)
{
    std::ifstream input(path);
    if (!input.is_open()) {
        return {};
    }
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

bool awaitable_source_has_no_abort()
{
    const auto path = std::filesystem::path(GALAY_SOURCE_ROOT) /
        "galay-kernel" / "core" / "awaitable.h";
    const auto source = read_all(path);
    if (source.empty()) {
        std::cerr << "[T133] failed to read " << path << '\n';
        return false;
    }
    if (source.find("std::abort(") != std::string::npos) {
        std::cerr << "[T133] awaitable.h must not call std::abort() on public error paths\n";
        return false;
    }
    return true;
}

int run_overflow_case()
{
    IOController controller(GHandle::invalid());
    BoundaryFlow flow;
    BoundaryStep first(&flow);
    BoundaryStep second(&flow);
    SequenceAwaitable<BoundaryResult, 1> sequence(&controller);

    sequence.queue(first);
    sequence.queue(second);

    if (!sequence.await_ready()) {
        std::cerr << "[T133] overflowed sequence should be ready with an error\n";
        return 2;
    }

    auto result = sequence.await_resume();
    if (result.has_value()) {
        std::cerr << "[T133] overflowed sequence unexpectedly succeeded\n";
        return 3;
    }
    if (!IOError::contains(result.error().code(), kParamInvalid)) {
        std::cerr << "[T133] overflowed sequence returned wrong error code\n";
        return 4;
    }
    return 0;
}

bool child_case_passes(const char* self)
{
    const pid_t child = fork();
    if (child < 0) {
        std::cerr << "[T133] fork failed: " << std::strerror(errno) << '\n';
        return false;
    }
    if (child == 0) {
        execl(self, self, "--overflow", static_cast<char*>(nullptr));
        std::cerr << "[T133] execl failed: " << std::strerror(errno) << '\n';
        _exit(127);
    }

    int status = 0;
    if (waitpid(child, &status, 0) < 0) {
        std::cerr << "[T133] waitpid failed: " << std::strerror(errno) << '\n';
        return false;
    }
    if (WIFSIGNALED(status)) {
        std::cerr << "[T133] overflow case terminated by signal "
                  << WTERMSIG(status) << '\n';
        return false;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        std::cerr << "[T133] overflow case exit code "
                  << (WIFEXITED(status) ? WEXITSTATUS(status) : -1) << '\n';
        return false;
    }
    return true;
}

bool domain_claim_failure_passes()
{
    TestScheduler scheduler;
    scheduler.start();

    DomainClaimState state;
    if (!schedule_task(scheduler, domain_claim_failure_task(&state))) {
        std::cerr << "[T133] failed to schedule domain claim failure task\n";
        scheduler.stop();
        return false;
    }

    const bool completed = wait_until_done(state.done);
    scheduler.stop();
    if (!completed) {
        std::cerr << "[T133] domain claim failure task did not complete\n";
        return false;
    }
    if (state.error.load(std::memory_order_acquire) !=
        static_cast<int>(BoundaryDomainError::kReadClaimRejected)) {
        std::cerr << "[T133] domain claim failure returned wrong domain error\n";
        return false;
    }
    return true;
}

} // namespace

int main(int argc, char* argv[])
{
    if (argc == 2 && std::strcmp(argv[1], "--overflow") == 0) {
        return run_overflow_case();
    }

    bool ok = true;
    ok = child_case_passes(argv[0]) && ok;
    ok = domain_claim_failure_passes() && ok;
    ok = awaitable_source_has_no_abort() && ok;
    return ok ? 0 : 1;
}
