/**
 * @file b19_resume_token_waker_ops.cc
 * @brief 测量 ResumeToken-backed Waker 的 hook 请求与拷贝成本。
 */

#include <galay/cpp/galay-kernel/core/scheduler.hpp>
#include <galay/cpp/galay-kernel/core/waker.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>

using namespace galay::kernel;

namespace {

constexpr int kIterations = 1'000'000;

class NullScheduler final : public Scheduler {
public:
    std::expected<void, IOError> start() override { return {}; }
    void stop() override {}
    bool schedule(TaskRef) override { return false; }
    bool scheduleDeferred(TaskRef) override { return false; }
    bool scheduleImmediately(TaskRef) override { return false; }
    bool addTimer(Timer::ptr) override { return false; }
    SchedulerType type() override { return kComputeScheduler; }
};

struct FakeResumeTokenState {
    detail::ResumeTokenHeader header;
    Scheduler* owner = nullptr;
    std::atomic<uint64_t> retain_count{0};
    std::atomic<uint64_t> release_count{0};
    std::atomic<uint64_t> request_count{0};
};

Scheduler* fakeOwner(void* state) noexcept {
    return static_cast<FakeResumeTokenState*>(state)->owner;
}

bool fakeRequestResume(void* state) noexcept {
    static_cast<FakeResumeTokenState*>(state)->request_count.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void fakeRetain(void* state) noexcept {
    static_cast<FakeResumeTokenState*>(state)->retain_count.fetch_add(1, std::memory_order_relaxed);
}

void fakeRelease(void* state) noexcept {
    static_cast<FakeResumeTokenState*>(state)->release_count.fetch_add(1, std::memory_order_relaxed);
}

constexpr detail::ResumeTokenHooks kFakeHooks{
    .owner_scheduler = fakeOwner,
    .request_resume = fakeRequestResume,
    .retain = fakeRetain,
    .release = fakeRelease,
};

detail::ResumeToken makeToken(FakeResumeTokenState* state) {
    state->header.hooks = &kFakeHooks;
    return detail::ResumeToken::fromCCoroutine(state);
}

double elapsedOpsPerSecond(std::chrono::steady_clock::time_point start,
                           std::chrono::steady_clock::time_point end,
                           int iterations) {
    const auto elapsed_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    return elapsed_ns > 0
        ? static_cast<double>(iterations) * 1'000'000'000.0 / static_cast<double>(elapsed_ns)
        : 0.0;
}

}  // namespace

int main() {
    NullScheduler scheduler;
    FakeResumeTokenState wake_only{.owner = &scheduler};
    {
        Waker waker(makeToken(&wake_only));
        const auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < kIterations; ++i) {
            waker.wakeUp();
        }
        const auto end = std::chrono::steady_clock::now();
        std::cout << "ResumeTokenWakerOps wake_only_iterations=" << kIterations
                  << ", wake_only_qps=" << std::fixed << std::setprecision(0)
                  << elapsedOpsPerSecond(start, end, kIterations) << "\n";
    }

    FakeResumeTokenState copy_wake{.owner = &scheduler};
    {
        Waker base(makeToken(&copy_wake));
        const auto start = std::chrono::steady_clock::now();
        for (int i = 0; i < kIterations; ++i) {
            Waker copy = base;
            copy.wakeUp();
        }
        const auto end = std::chrono::steady_clock::now();
        std::cout << "ResumeTokenWakerOps copy_wake_iterations=" << kIterations
                  << ", copy_wake_qps=" << std::fixed << std::setprecision(0)
                  << elapsedOpsPerSecond(start, end, kIterations) << "\n";
    }

    const bool counts_ok =
        wake_only.request_count.load(std::memory_order_acquire) == kIterations &&
        wake_only.retain_count.load(std::memory_order_acquire) == 1 &&
        wake_only.release_count.load(std::memory_order_acquire) == 1 &&
        copy_wake.request_count.load(std::memory_order_acquire) == kIterations &&
        copy_wake.retain_count.load(std::memory_order_acquire) == static_cast<uint64_t>(kIterations + 1) &&
        copy_wake.release_count.load(std::memory_order_acquire) == static_cast<uint64_t>(kIterations + 1);
    if (!counts_ok) {
        std::cerr << "[B19] unexpected hook counts"
                  << ", wake_request=" << wake_only.request_count.load(std::memory_order_acquire)
                  << ", wake_retain=" << wake_only.retain_count.load(std::memory_order_acquire)
                  << ", wake_release=" << wake_only.release_count.load(std::memory_order_acquire)
                  << ", copy_request=" << copy_wake.request_count.load(std::memory_order_acquire)
                  << ", copy_retain=" << copy_wake.retain_count.load(std::memory_order_acquire)
                  << ", copy_release=" << copy_wake.release_count.load(std::memory_order_acquire)
                  << "\n";
        return 1;
    }

    std::cout << "ResumeTokenWakerOps PASS"
              << ", wake_requests=" << wake_only.request_count.load(std::memory_order_acquire)
              << ", wake_retains=" << wake_only.retain_count.load(std::memory_order_acquire)
              << ", copy_requests=" << copy_wake.request_count.load(std::memory_order_acquire)
              << ", copy_retains=" << copy_wake.retain_count.load(std::memory_order_acquire)
              << ", copy_releases=" << copy_wake.release_count.load(std::memory_order_acquire)
              << "\n";
    return 0;
}
