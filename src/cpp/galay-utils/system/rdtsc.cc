#include "rdtsc.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <thread>

#if (defined(__GNUC__) || defined(__clang__)) && defined(__x86_64__)
#include <cpuid.h>
#if defined(__linux__)
#include <sys/prctl.h>
#include <time.h>
#endif
#endif

namespace galay::utils {
namespace {

#if (defined(__GNUC__) || defined(__clang__)) && defined(__x86_64__)
struct CounterStamp {
    std::uint64_t ticks;
    std::uint32_t cpu; ///< RDTSCP 返回的 TSC_AUX 标记，用于检查采样期间是否迁核。
};

CounterStamp read_stamp() noexcept {
    std::uint32_t low;
    std::uint32_t high;
    std::uint32_t cpu;
    __asm__ volatile("rdtscp\n\tlfence" : "=a"(low), "=d"(high), "=c"(cpu) : : "memory");
    return {(static_cast<std::uint64_t>(high) << 32U) | low, cpu};
}

std::expected<std::int64_t, std::error_code> reference_ns() {
#if defined(__linux__)
    timespec now{};
    if (clock_gettime(CLOCK_MONOTONIC_RAW, &now) != 0) {
        return std::unexpected(std::error_code(errno, std::generic_category()));
    }
    return static_cast<std::int64_t>(now.tv_sec) * 1'000'000'000 + now.tv_nsec;
#else
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
#endif
}

std::expected<std::uint64_t, std::error_code> calibrate_frequency() {
    // 采集五轮约 20 ms 的区间，取中位数以减小调度和读取延迟造成的偏差。
    std::array<std::uint64_t, 5> frequencies{};
    for (auto& frequency : frequencies) {
        const auto begin_before = read_stamp();
        const auto begin_ns = reference_ns();
        const auto begin_after = read_stamp();
        if (!begin_ns) {
            return std::unexpected(begin_ns.error());
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
        const auto end_before = read_stamp();
        const auto end_ns = reference_ns();
        const auto end_after = read_stamp();
        if (!end_ns) {
            return std::unexpected(end_ns.error());
        }
        if (begin_before.cpu != begin_after.cpu || begin_before.cpu != end_before.cpu ||
            begin_before.cpu != end_after.cpu || begin_after.ticks < begin_before.ticks ||
            end_before.ticks < begin_after.ticks || end_after.ticks < end_before.ticks ||
            *end_ns <= *begin_ns) {
            return std::unexpected(std::make_error_code(std::errc::resource_unavailable_try_again));
        }
        // 用参考时钟读取前后的 tick 中点对齐采样，减小参考时钟自身读取延迟的偏差。
        const auto begin = begin_before.ticks + (begin_after.ticks - begin_before.ticks) / 2;
        const auto end = end_before.ticks + (end_after.ticks - end_before.ticks) / 2;
        using Wide = unsigned __int128;
        frequency = static_cast<std::uint64_t>(
            static_cast<Wide>(end - begin) * 1'000'000'000 / (*end_ns - *begin_ns));
    }
    std::sort(frequencies.begin(), frequencies.end());
    const auto median = frequencies[frequencies.size() / 2];
    // 极差超过中位数的 0.5% 时拒绝样本；该阈值只筛除不稳定采样，不保证计时精度。
    if (median == 0 || frequencies.back() - frequencies.front() > median / 200) {
        return std::unexpected(std::make_error_code(std::errc::resource_unavailable_try_again));
    }
    return median;
}
#endif

} // namespace

std::expected<Rdtsc, std::error_code> Rdtsc::create() {
#if !defined(__SIZEOF_INT128__)
    return std::unexpected(std::make_error_code(std::errc::operation_not_supported));
#else
    std::uint64_t frequency = 0;
    bool invariant = false;
    auto source = RdtscFrequencySource::kArchitectural;
#if (defined(__GNUC__) || defined(__clang__)) && defined(__x86_64__)
    unsigned eax;
    unsigned ebx;
    unsigned ecx;
    unsigned edx;
    // 热路径分别依赖 TSC、SSE2 提供的 LFENCE 和 RDTSCP，初始化时一次性检查。
    if (!__get_cpuid(1, &eax, &ebx, &ecx, &edx) || !(edx & (1U << 4U)) ||
        !(edx & (1U << 26U)) ||
        !__get_cpuid(0x80000001U, &eax, &ebx, &ecx, &edx) || !(edx & (1U << 27U))) {
        return std::unexpected(std::make_error_code(std::errc::operation_not_supported));
    }
#if defined(__linux__)
    int permission = 0;
    if (prctl(PR_GET_TSC, &permission) != 0) {
        return std::unexpected(std::error_code(errno, std::generic_category()));
    }
    if (permission != PR_TSC_ENABLE) {
        return std::unexpected(std::make_error_code(std::errc::operation_not_permitted));
    }
#endif
    if (__get_cpuid(0x80000007U, &eax, &ebx, &ecx, &edx)) {
        invariant = (edx & (1U << 8U)) != 0;
    }
    // CPUID 0x15：EAX/EBX 为计数器与晶振频率比的分母/分子，ECX 为晶振频率（Hz）。
    if (__get_cpuid_count(0x15U, 0, &eax, &ebx, &ecx, &edx) && eax && ebx && ecx) {
        frequency = static_cast<std::uint64_t>(ecx) * ebx / eax;
    }
    if (frequency == 0) {
        const auto calibrated = calibrate_frequency();
        if (!calibrated) {
            return std::unexpected(calibrated.error());
        }
        frequency = *calibrated;
        source = RdtscFrequencySource::kCalibrated;
    }
#elif (defined(__GNUC__) || defined(__clang__)) && defined(__aarch64__)
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(frequency));
    invariant = true;
#else
    return std::unexpected(std::make_error_code(std::errc::operation_not_supported));
#endif
    using Wide = unsigned __int128;
    if (frequency == 0) {
        return std::unexpected(std::make_error_code(std::errc::operation_not_supported));
    }
    // 初始化时计算带 32 位小数的纳秒/tick 乘数，读取后的换算只需乘法和移位。
    const Wide multiplier = (static_cast<Wide>(1'000'000'000) << 32U) / frequency;
    if (multiplier == 0 || multiplier > std::numeric_limits<std::uint64_t>::max()) {
        return std::unexpected(std::make_error_code(std::errc::value_too_large));
    }
    return Rdtsc{frequency, static_cast<std::uint64_t>(multiplier), source, invariant};
#endif
}

} // namespace galay::utils
