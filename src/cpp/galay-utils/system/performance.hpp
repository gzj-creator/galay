#ifndef GALAY_UTILS_PERFORMANCE_HPP
#define GALAY_UTILS_PERFORMANCE_HPP

#include <array>
#include <cerrno>
#include <expected>
#include <span>
#include <system_error>
#include <vector>

#if defined(__linux__)
#include <linux/mempolicy.h>
#include <sched.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace galay::utils {

// Values match the Linux memory-policy ABI. Other platforms return ENOTSUP.
enum class NumaPolicy { Default = 0, Bind = 2, Interleave = 3 };

struct NumaPolicyState {
    // Preserve the native mode, including flags and inherited policies that
    // this interface does not set (for example MPOL_PREFERRED).
    int native_mode = 0;
    std::vector<unsigned> nodes;
};

/**
 * @brief 当前线程的 CPU / NUMA 放置控制，不修改系统全局设置。
 * @details 在线程创建及被测数据分配之前调用；Linux 子线程继承 CPU mask
 *          和内存策略。内存策略影响后续首次触碰的页，不迁移已有内存。
 *          不支持的平台明确返回 operation_not_supported。所有系统错误
 *          保留原始 errno；本类不静默降级、不修改频率或调度优先级。
 */
class Performance {
public:
#if defined(__linux__)
    static constexpr unsigned kMaxCpus = CPU_SETSIZE;
#else
    static constexpr unsigned kMaxCpus = 1024;
#endif
    static constexpr unsigned kMaxNumaNodes = 1024;

    /** @brief 读取调用线程的有效 CPU mask，而非进程主线程的 mask。
     *  @note mask 容量不足时由内核返回 EINVAL，不截断或猜测 CPU 编号。 */
    [[nodiscard]] static std::expected<std::vector<unsigned>, std::error_code> cpuAffinity()
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

    /** @brief 设置当前线程并返回回读的有效 mask，重复 CPU ID 自动合并。
     *  @note 内核可能与 cpuset / online mask 取交集；调用方必须检查返回
     *        集合是否等于请求。需要尊重继承的 taskset 边界时先验证子集。
     *        回读失败时绑定可能已生效；此接口不承诺事务式回滚。 */
    [[nodiscard]] static std::expected<std::vector<unsigned>, std::error_code>
    bindCurrentThread(std::span<const unsigned> cpus)
    {
        if (cpus.empty()) {
            return std::unexpected(std::make_error_code(std::errc::invalid_argument));
        }
        for (unsigned cpu : cpus) {
            if (cpu >= kMaxCpus) {
                return std::unexpected(std::make_error_code(std::errc::invalid_argument));
            }
        }
#if defined(__linux__)
        cpu_set_t mask;
        CPU_ZERO(&mask);
        for (unsigned cpu : cpus) {
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

    /** @brief 读取 cpuset 允许的内存节点，不将所有在线节点视为可用。 */
    [[nodiscard]] static std::expected<std::vector<unsigned>, std::error_code> allowedNumaNodes()
    {
#if defined(__linux__)
        NodeMask mask{};
        if (syscall(SYS_get_mempolicy, nullptr, mask.data(),
                    static_cast<unsigned long>(kMaxNumaNodes), nullptr,
                    static_cast<unsigned long>(MPOL_F_MEMS_ALLOWED)) != 0) {
            return std::unexpected(std::error_code(errno, std::generic_category()));
        }
        return nodesFromMask(mask);
#else
        return std::unexpected(std::make_error_code(std::errc::operation_not_supported));
#endif
    }

    [[nodiscard]] static std::expected<NumaPolicyState, std::error_code> numaPolicy()
    {
#if defined(__linux__)
        NodeMask mask{};
        NumaPolicyState state;
        if (syscall(SYS_get_mempolicy, &state.native_mode, mask.data(),
                    static_cast<unsigned long>(kMaxNumaNodes), nullptr, 0UL) != 0) {
            return std::unexpected(std::error_code(errno, std::generic_category()));
        }
        state.nodes = nodesFromMask(mask);
        return state;
#else
        return std::unexpected(std::make_error_code(std::errc::operation_not_supported));
#endif
    }

    /** @brief 设置后续内存分配策略；调用方可用 numaPolicy() 核验。
     *  @note Default 必须使用空节点集；Bind / Interleave 必须使用非空集。
     *        不将请求失败静默降级，不迁移已分配的页。 */
    [[nodiscard]] static std::expected<void, std::error_code>
    setNumaPolicy(NumaPolicy policy, std::span<const unsigned> nodes)
    {
        if ((policy != NumaPolicy::Default && policy != NumaPolicy::Bind &&
             policy != NumaPolicy::Interleave) ||
            (policy == NumaPolicy::Default ? !nodes.empty() : nodes.empty())) {
            return std::unexpected(std::make_error_code(std::errc::invalid_argument));
        }
        for (unsigned node : nodes) {
            if (node >= kMaxNumaNodes) {
                return std::unexpected(std::make_error_code(std::errc::invalid_argument));
            }
        }
#if defined(__linux__)
        NodeMask mask{};
        for (unsigned node : nodes) {
            mask[node / kWordBits] |= 1UL << (node % kWordBits);
        }
        if (syscall(SYS_set_mempolicy, static_cast<int>(policy),
                    nodes.empty() ? nullptr : mask.data(),
                    nodes.empty() ? 0UL : static_cast<unsigned long>(kMaxNumaNodes)) != 0) {
            return std::unexpected(std::error_code(errno, std::generic_category()));
        }
        return {};
#else
        return std::unexpected(std::make_error_code(std::errc::operation_not_supported));
#endif
    }

private:
    static constexpr unsigned kWordBits = sizeof(unsigned long) * 8;
    using NodeMask = std::array<unsigned long, kMaxNumaNodes / kWordBits>;

    static std::vector<unsigned> nodesFromMask(const NodeMask& mask)
    {
        std::vector<unsigned> nodes;
        for (unsigned node = 0; node < kMaxNumaNodes; ++node) {
            if ((mask[node / kWordBits] & (1UL << (node % kWordBits))) != 0) {
                nodes.push_back(node);
            }
        }
        return nodes;
    }
};

} // namespace galay::utils

#endif
