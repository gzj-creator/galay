/**
 * @file t137_c_coro_resume_token.cc
 * @brief 验证真实 C coroutine task 暴露 ResumeToken/Waker 边界。
 */

#include <galay/c/galay-kernel-c/core-c/runtime_c.h>
#include <galay/c/galay-kernel-c/coro-c/coro_task_c.h>
#include <galay/cpp/galay-kernel/core/runtime.h>
#include <galay/cpp/galay-kernel/core/scheduler.hpp>
#include <galay/cpp/galay-kernel/core/waker.h>

#include "src/c/galay-kernel-c/coro-c/coro_task_internal.hpp"

#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>

using namespace std::chrono_literals;

namespace {

struct TokenEntryState {
    std::atomic<int> phase{0};
    galay_coro_task_t current{nullptr};
};

bool waitUntil(auto&& predicate,
               std::chrono::milliseconds timeout = 1500ms,
               std::chrono::milliseconds step = 1ms)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(step);
    }
    return predicate();
}

void token_entry(void* arg)
{
    auto* state = static_cast<TokenEntryState*>(arg);
    if (galay_coro_current(&state->current).code == C_IOResultOk) {
        state->phase.store(1, std::memory_order_release);
    }
    (void)galay_coro_yield();
    state->phase.store(2, std::memory_order_release);
}

galay::kernel::Task<void> scheduler_join_probe(galay_coro_task_t* task,
                                               std::atomic<int>* observed_code)
{
    C_IOResult result = galay_coro_join(task, 0);
    observed_code->store(static_cast<int>(result.code), std::memory_order_release);
    co_return;
}

}  // namespace

int main()
{
    C_RuntimeConfig config = galay_kernel_runtime_config_default();
    config.io_scheduler_count = 1;
    config.compute_scheduler_count = 1;

    galay_kernel_runtime_t runtime{};
    if (galay_kernel_runtime_create(&config, &runtime) != C_RuntimeSuccess ||
        galay_kernel_runtime_start(&runtime) != C_RuntimeSuccess) {
        std::cerr << "[T137] failed to create/start C runtime\n";
        return 1;
    }

    TokenEntryState state;
    galay_coro_task_t task{};
    if (galay_coro_spawn(&runtime, token_entry, &state, nullptr, &task).code != C_IOResultOk) {
        std::cerr << "[T137] failed to spawn C coroutine\n";
        return 2;
    }

    if (!waitUntil([&]() { return state.phase.load(std::memory_order_acquire) >= 1; })) {
        std::cerr << "[T137] C coroutine did not expose current task\n";
        return 3;
    }

    auto* cpp_runtime = static_cast<galay::kernel::Runtime*>(runtime.runtime);
    auto* owner = cpp_runtime->getIOScheduler(0);
    {
        galay::kernel::Waker waker(
            galay::kernel::coro_c::makeResumeToken(state.current));
        if (waker.getScheduler() != owner) {
            std::cerr << "[T137] C coroutine ResumeToken should expose owner scheduler\n";
            return 4;
        }

        galay::kernel::Waker copy = waker;
        waker.wakeUp();
        copy.wakeUp();
    }

    if (galay_coro_join(&task, 1000).code != C_IOResultOk ||
        !waitUntil([&]() { return state.phase.load(std::memory_order_acquire) == 2; })) {
        std::cerr << "[T137] C coroutine did not finish after token wake path\n";
        return 5;
    }

    std::atomic<int> scheduler_join_code{-1};
    auto* compute = cpp_runtime->getComputeScheduler(0);
    if (compute == nullptr ||
        !galay::kernel::scheduleTask(*compute, scheduler_join_probe(&task, &scheduler_join_code)) ||
        !waitUntil([&]() {
            return scheduler_join_code.load(std::memory_order_acquire) >= 0;
        }) ||
        scheduler_join_code.load(std::memory_order_acquire) !=
            static_cast<int>(C_IOResultInvalid)) {
        std::cerr << "[T137] C coroutine join should be rejected from scheduler thread\n";
        return 6;
    }

    if (galay_coro_destroy(&state.current).code != C_IOResultOk ||
        galay_coro_destroy(&task).code != C_IOResultOk) {
        std::cerr << "[T137] failed to release C coroutine handles\n";
        return 7;
    }

    (void)galay_kernel_runtime_stop(&runtime);
    if (galay_kernel_runtime_destroy(&runtime) != C_RuntimeSuccess) {
        return 8;
    }

    std::cout << "T137-CCoroResumeToken PASS\n";
    return 0;
}
