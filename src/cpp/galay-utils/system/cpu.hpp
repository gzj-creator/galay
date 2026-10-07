#ifndef GALAY_UTILS_SYSTEM_CPU_HPP
#define GALAY_UTILS_SYSTEM_CPU_HPP

#include "detail/sysfs.hpp"

#include <cerrno>
#include <cstddef>
#include <expected>
#include <limits>
#include <memory>
#include <span>
#include <system_error>
#include <thread>
#include <vector>

#if defined(__linux__)
#include <sched.h>
#endif

namespace galay::utils {

#if defined(__linux__)
namespace detail {

struct CpuMaskDeleter {
    void operator()(cpu_set_t* mask) const noexcept { CPU_FREE(mask); }
};

struct CpuMask {
    std::unique_ptr<cpu_set_t, CpuMaskDeleter> data;
    std::size_t size;
};

[[nodiscard]] inline std::expected<CpuMask, std::error_code> query_cpu_mask(pid_t pid)
{
    // sched_getaffinity returns EINVAL when the userspace mask is too small.
    // Grow by whole words, as documented by CPU_ALLOC(3), without relying on
    // hardware_concurrency or assuming that CPU IDs are contiguous.
    std::size_t capacity = CPU_SETSIZE;
    constexpr std::size_t maxCapacity = std::numeric_limits<int>::max();
    for (;;) {
        CpuMask mask{std::unique_ptr<cpu_set_t, CpuMaskDeleter>(CPU_ALLOC(capacity)),
                     CPU_ALLOC_SIZE(capacity)};
        if (!mask.data) {
            return std::unexpected(std::error_code(errno, std::generic_category()));
        }
        CPU_ZERO_S(mask.size, mask.data.get());
        if (sched_getaffinity(pid, mask.size, mask.data.get()) == 0) {
            return mask;
        }
        const auto error = std::error_code(errno, std::generic_category());
        if (error.value() != EINVAL || capacity == maxCapacity) {
            return std::unexpected(error);
        }
        capacity = std::min(capacity * 2, maxCapacity);
    }
}

[[nodiscard]] inline std::expected<std::vector<unsigned>, std::error_code>
query_cpu_affinity(pid_t pid)
{
    const auto mask = query_cpu_mask(pid);
    if (!mask) {
        return std::unexpected(mask.error());
    }
    std::vector<unsigned> cpus;
    for (unsigned cpu = 0; cpu < mask->size * 8; ++cpu) {
        if (CPU_ISSET_S(cpu, mask->size, mask->data.get())) {
            if (cpu >= static_cast<unsigned>(std::numeric_limits<int>::max())) {
                return std::unexpected(std::make_error_code(std::errc::value_too_large));
            }
            cpus.push_back(cpu);
        }
    }
    return cpus;
}

[[nodiscard]] inline std::expected<void, std::error_code>
set_cpu_affinity(pid_t pid, std::span<const unsigned> cpus)
{
    if (cpus.empty()) {
        return std::unexpected(std::make_error_code(std::errc::invalid_argument));
    }
    auto mask = query_cpu_mask(pid);
    if (!mask) {
        return std::unexpected(mask.error());
    }
    CPU_ZERO_S(mask->size, mask->data.get());
    for (const unsigned cpu : cpus) {
        if (cpu >= mask->size * 8 ||
            cpu >= static_cast<unsigned>(std::numeric_limits<int>::max())) {
            return std::unexpected(std::make_error_code(std::errc::invalid_argument));
        }
        CPU_SET_S(cpu, mask->size, mask->data.get());
    }
    if (sched_setaffinity(pid, mask->size, mask->data.get()) != 0) {
        return std::unexpected(std::error_code(errno, std::generic_category()));
    }
    return {};
}

} // namespace detail
#endif

/**
 * @brief 硬件并发度、在线 CPU ID 和当前线程的亲和性控制。
 * @details 只修改调用线程，不修改进程的其他线程或系统全局调度设置。
 */
class CPU {
public:
    /**
     * @brief 返回 std::thread::hardware_concurrency() 的硬件并发度提示。
     * @details 可能为 0；不是在线/有效 affinity 的大小，也不是 CPU ID 上界。
     */
    [[nodiscard]] static unsigned count() noexcept
    {
        return std::thread::hardware_concurrency();
    }

    /** @brief 查询系统在线 CPU ID 快照，不按当前线程的 cpuset 过滤。 */
    [[nodiscard]] static std::expected<std::vector<unsigned>, std::error_code>
    online_cpus()
    {
#if defined(__linux__)
        return detail::read_system_ids("/sys/devices/system/cpu/online",
                                    std::numeric_limits<int>::max());
#else
        return std::unexpected(std::make_error_code(std::errc::operation_not_supported));
#endif
    }

    /** @brief 查询瞬间正在执行调用线程的 CPU ID；不保证后续绑定或不迁移。 */
    [[nodiscard]] static std::expected<unsigned, std::error_code> current_id()
    {
#if defined(__linux__)
        const int cpu = sched_getcpu();
        if (cpu < 0) {
            return std::unexpected(std::error_code(errno, std::generic_category()));
        }
        if (cpu == std::numeric_limits<int>::max()) {
            return std::unexpected(std::make_error_code(std::errc::value_too_large));
        }
        return static_cast<unsigned>(cpu);
#else
        return std::unexpected(std::make_error_code(std::errc::operation_not_supported));
#endif
    }

    /** @brief 读取调用线程的有效 CPU mask，使用动态容量，不受 CPU_SETSIZE 限制。 */
    [[nodiscard]] static std::expected<std::vector<unsigned>, std::error_code>
    cpu_affinity()
    {
#if defined(__linux__)
        return detail::query_cpu_affinity(0);
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
    bind_current_thread(std::span<const unsigned> cpus)
    {
#if defined(__linux__)
        const auto set = detail::set_cpu_affinity(0, cpus);
        if (!set) {
            return std::unexpected(set.error());
        }
        return cpu_affinity();
#else
        (void)cpus;
        return std::unexpected(std::make_error_code(std::errc::operation_not_supported));
#endif
    }
};

} // namespace galay::utils

#endif // GALAY_UTILS_SYSTEM_CPU_HPP
