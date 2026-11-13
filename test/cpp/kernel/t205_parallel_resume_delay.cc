/**
 * @file t205_parallel_resume_delay.cc
 * @brief 复现 ParallelScheduler 空闲时跨线程恢复的延迟缺陷。
 *
 * @details worker 空闲后阻塞在 1 ms 的 `wait_dequeue_timed`；恢复请求只写入
 * `m_resumeQueue`，不会唤醒该等待。测试取 12 次中位数并要求不超过 400 us。
 * 当前实现预计每次等待接近 1 ms，因此本测试当前失败。生产修复后移除 CMake
 * 中的 DISABLED 属性，使该用例进入常规回归。
 */

#include <galay/cpp/galay-kernel/core/task.h>
#include <galay/cpp/galay-kernel/parallel/parallel_scheduler.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <thread>

using namespace galay::kernel;
using namespace std::chrono_literals;

namespace {

constexpr size_t kSampleCount = 12;
constexpr int64_t kMaximumMedianDelayNanoseconds = 400'000;

/** @brief 保存停泊任务、请求时间与恢复耗时样本。 */
struct ResumeDelayState {
    std::atomic<int> parked_generation{0};  ///< 已停泊样本代数
    std::atomic<int> resumed_generation{0};  ///< 已恢复样本代数
    std::atomic<int64_t> request_nanoseconds{0};  ///< 最近一次恢复请求时间
    std::atomic<bool> done{false};  ///< 受测任务是否完成
    TaskRef parked_task;  ///< 最近一次停泊的任务引用
    std::array<int64_t, kSampleCount> delays{};  ///< 每次请求至恢复的耗时
};

/**
 * @brief 等待给定条件，直至超时。
 * @param predicate 条件回调
 * @param timeout 最长等待时间
 * @return 条件在超时前满足时返回 true
 */
bool wait_until(auto&& predicate, std::chrono::milliseconds timeout = 3s)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::yield();
    }
    return predicate();
}

/** @brief 返回 steady_clock 的纳秒计数。 */
int64_t steady_now_nanoseconds()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

/** @brief 发布当前任务引用后一直停泊到外部恢复请求。 */
struct ParkUntilResumeAwaiter {
    ResumeDelayState* state;  ///< 共享测量状态
    size_t sample;  ///< 当前样本索引

    bool await_ready() const noexcept { return false; }

    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> handle) noexcept
    {
        state->parked_task = handle.promise().task_ref_view();
        state->parked_generation.store(static_cast<int>(sample + 1),
                                       std::memory_order_release);
        return true;
    }

    void await_resume() noexcept
    {
        const int64_t resumed_nanoseconds = steady_now_nanoseconds();
        state->delays[sample] = resumed_nanoseconds -
            state->request_nanoseconds.load(std::memory_order_acquire);
        state->resumed_generation.store(static_cast<int>(sample + 1),
                                        std::memory_order_release);
    }
};

/** @brief 连续停泊并记录跨线程恢复延迟。 */
Task<void> measured_resume_task(ResumeDelayState* state)
{
    for (size_t sample = 0; sample < kSampleCount; ++sample) {
        co_await ParkUntilResumeAwaiter{state, sample};
    }
    state->done.store(true, std::memory_order_release);
    co_return;
}

/**
 * @brief 测量空闲 ParallelScheduler 的跨线程恢复延迟中位数。
 * @return 所有恢复成功且中位数未超过阈值时返回 true
 */
bool run_resume_delay_scenario()
{
    ParallelScheduler scheduler;
    const auto started = scheduler.start();
    if (!started) {
        std::cerr << "[T205] scheduler start failed\n";
        return false;
    }

    ResumeDelayState state;
    if (!schedule_task(scheduler, measured_resume_task(&state))) {
        std::cerr << "[T205] failed to schedule measured task\n";
        scheduler.stop();
        return false;
    }

    bool requests_ok = true;
    for (size_t sample = 0; sample < kSampleCount; ++sample) {
        const int expected_generation = static_cast<int>(sample + 1);
        if (!wait_until([&]() {
                return state.parked_generation.load(std::memory_order_acquire) >=
                    expected_generation;
            })) {
            std::cerr << "[T205] task did not park for sample " << sample << '\n';
            requests_ok = false;
            break;
        }

        // 超过两轮超时确保 worker 已进入空闲等待；样本错开请求相位，
        // 避免每次都在 1 ms 定时边界前发出请求而掩盖等待延迟。
        const auto phase_offset = std::chrono::microseconds((sample * 173U) % 900U);
        std::this_thread::sleep_for(2ms + phase_offset);
        state.request_nanoseconds.store(steady_now_nanoseconds(),
                                        std::memory_order_release);
        if (detail::request_task_resume_state_detailed(state.parked_task.state()) !=
            detail::TaskResumeResult::kAccepted) {
            std::cerr << "[T205] resume request rejected for sample " << sample << '\n';
            requests_ok = false;
            break;
        }

        if (!wait_until([&]() {
                return state.resumed_generation.load(std::memory_order_acquire) >=
                    expected_generation;
            })) {
            std::cerr << "[T205] task did not resume for sample " << sample << '\n';
            requests_ok = false;
            break;
        }
    }

    const bool completed = requests_ok && wait_until([&]() {
        return state.done.load(std::memory_order_acquire);
    });
    scheduler.stop();
    if (!completed) {
        return false;
    }

    auto sorted_delays = state.delays;
    std::sort(sorted_delays.begin(), sorted_delays.end());
    const int64_t median_delay = sorted_delays[kSampleCount / 2];
    std::cout << "[T205] idle cross-thread resume median="
              << median_delay / 1'000 << "us max="
              << sorted_delays.back() / 1'000 << "us\n";

    if (median_delay > kMaximumMedianDelayNanoseconds) {
        std::cerr << "[T205] median resume delay exceeds 400us; the idle "
                     "wait_dequeue_timed path is the likely cause\n";
        return false;
    }
    return true;
}

}  // namespace

int main()
{
    return run_resume_delay_scenario() ? 0 : 1;
}
