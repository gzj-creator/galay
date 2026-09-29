/** @brief 分别测量禁止内联的调用成本、允许优化的派发路径，以及真实 Task 恢复。
 *  类型擦除路径额外包含 kind 判断，不能把与 CRTP 的差额算成 CRTP 成本。
 */
#include "../common/benchmark_environment.h"

#include <galay/cpp/galay-kernel/parallel/parallel_scheduler.h>
#include <galay/cpp/galay-kernel/core/scheduler_dispatch.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#if defined(__linux__)
#include <sched.h>
#endif

using namespace galay::kernel;

namespace {

template <typename T>
void doNotOptimize(const T& value) noexcept
{
#if defined(__GNUC__) || defined(__clang__)
    asm volatile("" : : "r,m"(value) : "memory");
#else
    (void)value;
#endif
}

class VirtualDispatch {
public:
    virtual ~VirtualDispatch() = default;
    virtual bool scheduleImmediately(uint64_t value) noexcept = 0;
};

class VirtualDispatchProbeA final : public VirtualDispatch {
public:
    [[gnu::noinline]] bool scheduleImmediately(uint64_t value) noexcept override
    {
        m_value += value | 1U;
        return true;
    }

    uint64_t value() const noexcept { return m_value; }

private:
    uint64_t m_padding = 0;
    uint64_t m_value = 0;
};

class VirtualDispatchProbeB final : public VirtualDispatch {
public:
    [[gnu::noinline]] bool scheduleImmediately(uint64_t value) noexcept override
    {
        m_value += value | 1U;
        return true;
    }

    uint64_t value() const noexcept { return m_value; }

private:
    uint64_t m_value = 0;
};

template <typename Derived>
class StaticDispatch {
public:
    static bool scheduleImmediately(Derived* scheduler, uint64_t value) noexcept
    {
        return scheduler->scheduleImmediatelyImpl(value);
    }
};

class OptVirtualProbe final : public VirtualDispatch {
public:
    bool scheduleImmediately(uint64_t value) noexcept override
    {
        m_value += value | 1U;
        return true;
    }

    uint64_t value() const noexcept { return m_value; }

private:
    uint64_t m_value = 0;
};

class CrtpDispatchProbe {
public:
    bool scheduleImmediatelyImpl(uint64_t value) noexcept
    {
        m_value += value | 1U;
        return true;
    }

    uint64_t value() const noexcept { return m_value; }

private:
    uint64_t m_value = 0;
};

class OptCrtpProbe {
public:
    bool scheduleImmediatelyImpl(uint64_t value) noexcept
    {
        m_value += value | 1U;
        return true;
    }

    uint64_t value() const noexcept { return m_value; }

private:
    uint64_t m_value = 0;
};

enum class SchedulerKind : uint8_t {
    kParallel,
    kIO,
};

struct ErasedCrtpDispatch {
    SchedulerKind kind;
    void* scheduler;
};

#if defined(__GNUC__) && !defined(__clang__)
__attribute__((noinline, noipa))
#else
[[gnu::noinline]]
#endif
bool scheduleVirtual(VirtualDispatch* scheduler, uint64_t value) noexcept
{
    return scheduler->scheduleImmediately(value);
}

bool scheduleCrtp(CrtpDispatchProbe* scheduler, uint64_t value) noexcept
{
    return StaticDispatch<CrtpDispatchProbe>::scheduleImmediately(scheduler, value);
}

#if defined(__GNUC__) && !defined(__clang__)
__attribute__((noinline, noipa))
#else
[[gnu::noinline]]
#endif
VirtualDispatch* selectVirtualProbe(bool use_alternate,
                                    VirtualDispatch* primary,
                                    VirtualDispatch* alternate) noexcept
{
    return use_alternate ? alternate : primary;
}

#if defined(__GNUC__) && !defined(__clang__)
__attribute__((noinline, noipa))
#else
[[gnu::noinline]]
#endif
bool scheduleErasedCrtp(ErasedCrtpDispatch scheduler, uint64_t value) noexcept
{
    if (scheduler.kind != SchedulerKind::kParallel) {
        return false;
    }
    return StaticDispatch<CrtpDispatchProbe>::scheduleImmediately(
        static_cast<CrtpDispatchProbe*>(scheduler.scheduler), value);
}

#if defined(__GNUC__) && !defined(__clang__)
__attribute__((noinline, noipa))
#else
[[gnu::noinline]]
#endif
double measureOptVirtualNs(VirtualDispatch* scheduler, size_t iterations)
{
    const auto begin = std::chrono::steady_clock::now();
    for (size_t i = 0; i < iterations; ++i) {
        if (!scheduler->scheduleImmediately(i)) {
            return -1;
        }
    }
    const auto end = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::nano>(end - begin).count() / iterations;
}

#if defined(__GNUC__) && !defined(__clang__)
__attribute__((noinline, noipa))
#else
[[gnu::noinline]]
#endif
double measureOptCrtpNs(OptCrtpProbe* scheduler, size_t iterations)
{
    const auto begin = std::chrono::steady_clock::now();
    for (size_t i = 0; i < iterations; ++i) {
        if (!StaticDispatch<OptCrtpProbe>::scheduleImmediately(scheduler, i)) {
            return -1;
        }
    }
    const auto end = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::nano>(end - begin).count() / iterations;
}

Task<void> increment(uint64_t& completed)
{
    ++completed;
    co_return;
}

struct SampleSummary {
    double median_ns;
    double p95_ns;
};

Task<void> countResumes(uint64_t& completed, size_t iterations)
{
    for (size_t i = 0; i < iterations; ++i) {
        ++completed;
        co_yield false;
    }
}

// Allocate once outside timing; each submission still owns its TaskRef and
// executes the real binding/resume protocol with normal compiler optimization.
template <typename SchedulerT>
double measureReusedTaskResumeNs(SchedulerT& scheduler, size_t iterations)
{
    uint64_t completed = 0;
    auto task = countResumes(completed, iterations);
    const TaskRef& ref = detail::TaskAccess::taskRef(task);
    detail::setTaskScheduler(ref, &scheduler);
    const auto begin = std::chrono::steady_clock::now();
    for (size_t i = 0; i < iterations; ++i) {
        if (!scheduler.scheduleImmediately(ref)) {
            return -1;
        }
    }
    const auto end = std::chrono::steady_clock::now();
    if (completed != iterations || !scheduler.scheduleImmediately(ref)) {
        return -1;
    }
    doNotOptimize(completed);
    return std::chrono::duration<double, std::nano>(end - begin).count() / iterations;
}

template <typename Function>
double measureCallNs(Function&& function, size_t iterations)
{
    const auto begin = std::chrono::steady_clock::now();
    for (size_t i = 0; i < iterations; ++i) {
        if (!function(i)) {
            return -1;
        }
    }
    const auto end = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::nano>(end - begin).count() / iterations;
}

template <typename SchedulerT>
double measureTaskResumeNs(SchedulerT& scheduler, size_t iterations)
{
    uint64_t completed = 0;
    const auto begin = std::chrono::steady_clock::now();
    for (size_t i = 0; i < iterations; ++i) {
        if (!scheduleTaskImmediately(scheduler, increment(completed))) {
            return -1;
        }
    }
    const auto end = std::chrono::steady_clock::now();
    if (completed != iterations) {
        return -1;
    }
    doNotOptimize(completed);
    return std::chrono::duration<double, std::nano>(end - begin).count() / iterations;
}

double measureErasedTaskResumeNs(Scheduler& scheduler, size_t iterations)
{
    uint64_t completed = 0;
    const auto begin = std::chrono::steady_clock::now();
    for (size_t i = 0; i < iterations; ++i) {
        Task<void> task = increment(completed);
        if (!scheduler.scheduleImmediately(detail::TaskAccess::detachTask(std::move(task)))) {
            return -1;
        }
    }
    const auto end = std::chrono::steady_clock::now();
    if (completed != iterations) {
        return -1;
    }
    doNotOptimize(completed);
    return std::chrono::duration<double, std::nano>(end - begin).count() / iterations;
}

template <typename SchedulerT>
double measureRejectedTaskNs(SchedulerT& scheduler, size_t iterations)
{
    const auto begin = std::chrono::steady_clock::now();
    for (size_t i = 0; i < iterations; ++i) {
        TaskRef invalid_task;
        if (scheduler.scheduleImmediately(std::move(invalid_task))) {
            return -1;
        }
    }
    const auto end = std::chrono::steady_clock::now();
    doNotOptimize(scheduler);
    return std::chrono::duration<double, std::nano>(end - begin).count() / iterations;
}

void printSummary(const char* name, SampleSummary summary)
{
    std::cout << name << " median_ns=" << summary.median_ns
              << " batch_mean_p95_ns=" << summary.p95_ns << '\n';
}

} // namespace

int main(int argc, char**)
{
    if (!galay::benchmark::initializeBenchmarkEnvironment()) {
        return 1;
    }

    constexpr size_t kWarmupIterations = 100'000;
    constexpr size_t kDispatchIterations = 4'000'000;
    constexpr size_t kTaskIterations = 200'000;
    constexpr size_t kRepetitions = 21;

    VirtualDispatchProbeA virtual_probe_a;
    VirtualDispatchProbeB virtual_probe_b;
    VirtualDispatch* virtual_probe = selectVirtualProbe(
        argc == 2, &virtual_probe_a, &virtual_probe_b);
    OptVirtualProbe opt_virtual_probe;
    VirtualDispatch* opt_virtual = selectVirtualProbe(false, &opt_virtual_probe, &virtual_probe_b);
    CrtpDispatchProbe crtp_probe;
    OptCrtpProbe opt_crtp_probe;
    ErasedCrtpDispatch erased_probe{SchedulerKind::kParallel, &crtp_probe};
    ParallelScheduler scheduler;
    Scheduler& erased_scheduler = scheduler;

    auto noinline_virtual = [&](size_t iterations) {
        return measureCallNs(
            [&](uint64_t value) { return scheduleVirtual(virtual_probe, value); }, iterations);
    };
    auto noinline_crtp = [&](size_t iterations) {
        return measureCallNs(
            [&](uint64_t value) { return scheduleCrtp(&crtp_probe, value); }, iterations);
    };
    auto erased_crtp = [&](size_t iterations) {
        return measureCallNs(
            [&](uint64_t value) { return scheduleErasedCrtp(erased_probe, value); }, iterations);
    };
    auto opt_virtual_call = [&](size_t iterations) {
        return measureOptVirtualNs(opt_virtual, iterations);
    };
    auto opt_crtp = [&](size_t iterations) {
        return measureOptCrtpNs(&opt_crtp_probe, iterations);
    };

    if (noinline_virtual(kWarmupIterations) < 0 || noinline_crtp(kWarmupIterations) < 0 ||
        erased_crtp(kWarmupIterations) < 0 || opt_virtual_call(kWarmupIterations) < 0 ||
        opt_crtp(kWarmupIterations) < 0 ||
        measureRejectedTaskNs(scheduler, kWarmupIterations) < 0 ||
        measureRejectedTaskNs(erased_scheduler, kWarmupIterations) < 0 ||
        measureTaskResumeNs(scheduler, kWarmupIterations) < 0 ||
        measureErasedTaskResumeNs(erased_scheduler, kWarmupIterations) < 0 ||
        measureReusedTaskResumeNs(scheduler, kWarmupIterations) < 0 ||
        measureReusedTaskResumeNs(erased_scheduler, kWarmupIterations) < 0) {
        return 1;
    }

    std::array<double, kRepetitions> noinline_virtual_samples{};
    std::array<double, kRepetitions> noinline_crtp_samples{};
    std::array<double, kRepetitions> erased_samples{};
    std::array<double, kRepetitions> opt_virtual_samples{};
    std::array<double, kRepetitions> opt_crtp_samples{};
    std::array<double, kRepetitions> concrete_reject_samples{};
    std::array<double, kRepetitions> erased_reject_samples{};
    std::array<double, kRepetitions> concrete_task_samples{};
    std::array<double, kRepetitions> erased_task_samples{};
    std::array<double, kRepetitions> concrete_reused_samples{};
    std::array<double, kRepetitions> erased_reused_samples{};
    for (size_t round = 0; round < kRepetitions; ++round) {
        // Rotate cases to avoid consistently favoring the first/last path.
        for (size_t step = 0; step < 11; ++step) {
            switch ((round + step) % 11) {
            case 0: noinline_virtual_samples[round] = noinline_virtual(kDispatchIterations); break;
            case 1: noinline_crtp_samples[round] = noinline_crtp(kDispatchIterations); break;
            case 2: erased_samples[round] = erased_crtp(kDispatchIterations); break;
            case 3: opt_virtual_samples[round] = opt_virtual_call(kDispatchIterations); break;
            case 4: opt_crtp_samples[round] = opt_crtp(kDispatchIterations); break;
            case 5: concrete_reject_samples[round] = measureRejectedTaskNs(scheduler, kDispatchIterations); break;
            case 6: erased_reject_samples[round] = measureRejectedTaskNs(erased_scheduler, kDispatchIterations); break;
            case 7: concrete_task_samples[round] = measureTaskResumeNs(scheduler, kTaskIterations); break;
            case 8: erased_task_samples[round] = measureErasedTaskResumeNs(erased_scheduler, kTaskIterations); break;
            case 9: concrete_reused_samples[round] = measureReusedTaskResumeNs(scheduler, kTaskIterations); break;
            case 10: erased_reused_samples[round] = measureReusedTaskResumeNs(erased_scheduler, kTaskIterations); break;
            }
        }
    }

    const auto summarize = [](std::array<double, kRepetitions> samples) {
        if (std::any_of(samples.begin(), samples.end(), [](double value) { return value < 0; })) {
            return SampleSummary{-1, -1};
        }
        std::sort(samples.begin(), samples.end());
        return SampleSummary{samples[kRepetitions / 2], samples[(kRepetitions * 95 + 99) / 100 - 1]};
    };
    const auto virtual_summary = summarize(noinline_virtual_samples);
    const auto crtp_summary = summarize(noinline_crtp_samples);
    const auto erased_crtp_summary = summarize(erased_samples);
    const auto opt_virtual_summary = summarize(opt_virtual_samples);
    const auto opt_crtp_summary = summarize(opt_crtp_samples);
    const auto concrete_reject_summary = summarize(concrete_reject_samples);
    const auto erased_reject_summary = summarize(erased_reject_samples);
    const auto concrete_task_summary = summarize(concrete_task_samples);
    const auto erased_task_summary = summarize(erased_task_samples);
    const auto concrete_reused_summary = summarize(concrete_reused_samples);
    const auto erased_reused_summary = summarize(erased_reused_samples);

    if (virtual_summary.median_ns < 0 || crtp_summary.median_ns < 0 ||
        erased_crtp_summary.median_ns < 0 || opt_virtual_summary.median_ns < 0 ||
        opt_crtp_summary.median_ns < 0 || concrete_reject_summary.median_ns < 0 ||
        erased_reject_summary.median_ns < 0 || concrete_task_summary.median_ns < 0 ||
        erased_task_summary.median_ns < 0 || concrete_reused_summary.median_ns < 0 ||
        erased_reused_summary.median_ns < 0) {
        return 1;
    }

    std::cout << std::fixed << std::setprecision(2);
#if defined(__linux__)
    std::cout << "cpu=" << sched_getcpu() << ' ';
#endif
    std::cout << "SchedulerDispatch raw_iterations=" << kDispatchIterations
              << " task_iterations=" << kTaskIterations
              << " repetitions=" << kRepetitions
              << " rotating_order=1\n";
    std::cout << "arithmetic reports throughput, not call latency; constant-empty rejection may optimize away; p95 is across batch means\n";
    printSummary("noinline_virtual_call", virtual_summary);
    printSummary("inline_crtp_throughput", crtp_summary);
    printSummary("noinline_erased_crtp_call", erased_crtp_summary);
    printSummary("opt_virtual_call", opt_virtual_summary);
    printSummary("opt_crtp_throughput", opt_crtp_summary);
    printSummary("constant_empty_reject_concrete", concrete_reject_summary);
    printSummary("constant_empty_reject_erased", erased_reject_summary);
    printSummary("concrete_task_resume", concrete_task_summary);
    printSummary("erased_task_resume", erased_task_summary);
    printSummary("concrete_reused_task_resume", concrete_reused_summary);
    printSummary("erased_reused_task_resume", erased_reused_summary);
    doNotOptimize(virtual_probe_a.value());
    doNotOptimize(virtual_probe_b.value());
    doNotOptimize(opt_virtual_probe.value());
    doNotOptimize(crtp_probe.value());
    doNotOptimize(opt_crtp_probe.value());
}
