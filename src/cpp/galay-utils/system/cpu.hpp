#ifndef GALAY_UTILS_SYSTEM_CPU_HPP
#define GALAY_UTILS_SYSTEM_CPU_HPP

#include <cerrno>
#include <expected>
#include <span>
#include <system_error>
#include <thread>
#include <vector>

#if defined(__linux__)
#include <sched.h>
#endif

namespace galay::utils {

/**
 * @brief 当前线程的 CPU 信息和亲和性控制。
 * @details 只修改调用线程，不修改进程的其他线程或系统全局调度设置。
 */
class CPU {
public:
#if defined(__linux__)
    static constexpr unsigned kMaxCpus = CPU_SETSIZE;
#else
    static constexpr unsigned kMaxCpus = 1024;
#endif

    /** @brief 返回标准库报告的逻辑 CPU 数量。 */
    [[nodiscard]] static unsigned count() noexcept
    {
        return std::thread::hardware_concurrency();
    }

    /** @brief 读取调用线程的有效 CPU mask。 */
    [[nodiscard]] static std::expected<std::vector<unsigned>, std::error_code>
    cpuAffinity()
    {
#if defined(__linux__)
        cpu_set_t mask;
        CPU_ZERO(&mask);
        if (sched_getaffinity(0, sizeof(mask), &mask) != 0) {
            return std::unexpected(std::error_code(errno, std::generic_category()));
        }
        std::vector<unsigned> cpus;
        for (unsigned cpu = 0; cpu < kMaxCpus; ++cpu) {
            if (CPU_ISSET(cpu, &mask)) {
                cpus.push_back(cpu);
            }
        }
        return cpus;
#else
        return std::unexpected(std::make_error_code(std::errc::operation_not_supported));
#endif
    }

    /**
     * @brief 设置调用线程的 CPU mask，并回读内核实际生效的 mask。
     * @details 重复 CPU ID 自动合并；调用方应比较返回值和请求集合，以发现
     *          cpuset、online CPU 等内核限制。回读失败时绑定可能已经生效。
     */
    [[nodiscard]] static std::expected<std::vector<unsigned>, std::error_code>
    bindCurrentThread(std::span<const unsigned> cpus)
    {
        if (cpus.empty()) {
            return std::unexpected(std::make_error_code(std::errc::invalid_argument));
        }
        for (const unsigned cpu : cpus) {
            if (cpu >= kMaxCpus) {
                return std::unexpected(std::make_error_code(std::errc::invalid_argument));
            }
        }
#if defined(__linux__)
        cpu_set_t mask;
        CPU_ZERO(&mask);
        for (const unsigned cpu : cpus) {
            CPU_SET(cpu, &mask);
        }
        if (sched_setaffinity(0, sizeof(mask), &mask) != 0) {
            return std::unexpected(std::error_code(errno, std::generic_category()));
        }
        return cpuAffinity();
#else
        return std::unexpected(std::make_error_code(std::errc::operation_not_supported));
#endif
    }
};

} // namespace galay::utils

#endif // GALAY_UTILS_SYSTEM_CPU_HPP
