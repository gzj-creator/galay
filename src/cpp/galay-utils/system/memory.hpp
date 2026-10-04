#ifndef GALAY_UTILS_SYSTEM_MEMORY_HPP
#define GALAY_UTILS_SYSTEM_MEMORY_HPP

#include "numa.hpp"

#include <array>
#include <cerrno>
#include <cstddef>
#include <expected>
#include <span>
#include <system_error>
#include <vector>

#if defined(__linux__)
#include <linux/mempolicy.h>
#include <sys/syscall.h>
#include <unistd.h>
#elif defined(__APPLE__)
#include <unistd.h>
#elif defined(_WIN32)
#include <windows.h>
#endif

namespace galay::utils {

/**
 * @brief 系统基础页大小和当前线程后续内存分配使用的默认 NUMA 内存策略。
 * @details 策略只影响后续新页的分配和首次触碰，不迁移已经存在的页；新建
 *          Linux 线程继承创建者当时的策略。
 */
class Memory {
public:
    static constexpr unsigned kMaxNodes = Numa::kMaxNodes;

    enum class Policy { Default = 0, Bind = 2, Interleave = 3 };

    struct PolicyState {
        // 保留 Linux 原生 mode 和 nodemask；RELATIVE_NODES 时 nodes 为相对
        // cpuset 的索引，而不是物理节点 ID。恢复时不得重新解释这个集合。
        int native_mode = 0;
        std::vector<unsigned> nodes;
    };

    /** @brief 返回以字节计的系统基础页大小（非 huge page 大小），必须为正数。 */
    [[nodiscard]] static std::expected<std::size_t, std::error_code> pageSize()
    {
#if defined(__linux__) || defined(__APPLE__)
        errno = 0;
        const long size = sysconf(_SC_PAGESIZE);
        if (size <= 0) {
            return std::unexpected(errno != 0
                ? std::error_code(errno, std::generic_category())
                : std::make_error_code(std::errc::io_error));
        }
        return static_cast<std::size_t>(size);
#elif defined(_WIN32)
        SYSTEM_INFO info{};
        GetSystemInfo(&info);
        if (info.dwPageSize == 0) {
            return std::unexpected(std::make_error_code(std::errc::io_error));
        }
        return static_cast<std::size_t>(info.dwPageSize);
#else
        return std::unexpected(std::make_error_code(std::errc::operation_not_supported));
#endif
    }

    /** @brief 读取调用线程当前的默认内存策略。 */
    [[nodiscard]] static std::expected<PolicyState, std::error_code> numaPolicy()
    {
#if defined(__linux__)
        NodeMask mask{};
        PolicyState state;
        if (syscall(SYS_get_mempolicy, &state.native_mode, mask.data(),
                    static_cast<unsigned long>(kMaxNodes), nullptr, 0UL) != 0) {
            return std::unexpected(std::error_code(errno, std::generic_category()));
        }
        state.nodes = nodesFromMask(mask);
        return state;
#else
        return std::unexpected(std::make_error_code(std::errc::operation_not_supported));
#endif
    }

    /**
     * @brief 设置调用线程后续内存分配的默认策略。
     * @details Default 要求空节点集；Bind 和 Interleave 要求非空节点集。
     */
    [[nodiscard]] static std::expected<void, std::error_code>
    setNumaPolicy(Policy policy, std::span<const unsigned> nodes)
    {
#if defined(__linux__)
        if ((policy != Policy::Default && policy != Policy::Bind &&
             policy != Policy::Interleave) ||
            (policy == Policy::Default ? !nodes.empty() : nodes.empty())) {
            return std::unexpected(std::make_error_code(std::errc::invalid_argument));
        }
#endif
        return setNativeNumaPolicy(static_cast<int>(policy), nodes);
    }

    /**
     * @brief 恢复保存的原生 mode（包括 flags）和节点集合，不降级为 Policy 枚举。
     * @details Default/Local 要求空集；Preferred 允许空集或非空集；其他
     *          原生模式要求非空集。原生 flags 和内核支持性由 Linux 校验。
     *          节点/cpuset 变化或内核权限限制可能导致恢复失败，必须检查结果。
     */
    [[nodiscard]] static std::expected<void, std::error_code>
    restoreNumaPolicy(const PolicyState& state)
    {
        return setNativeNumaPolicy(state.native_mode, state.nodes);
    }

private:
    [[nodiscard]] static std::expected<void, std::error_code>
    setNativeNumaPolicy(int nativeMode, std::span<const unsigned> nodes)
    {
#if defined(__linux__)
        const int mode = nativeMode & ~MPOL_MODE_FLAGS;
        const int flags = nativeMode & MPOL_MODE_FLAGS;
        if (mode < MPOL_DEFAULT || mode >= MPOL_MAX ||
            ((mode == MPOL_DEFAULT || mode == MPOL_LOCAL) && !nodes.empty()) ||
            (mode != MPOL_DEFAULT && mode != MPOL_LOCAL && mode != MPOL_PREFERRED && nodes.empty()) ||
            ((flags & MPOL_F_STATIC_NODES) && (flags & MPOL_F_RELATIVE_NODES))) {
            return std::unexpected(std::make_error_code(std::errc::invalid_argument));
        }
        if (((mode == MPOL_LOCAL) || (mode == MPOL_PREFERRED && nodes.empty())) &&
            (flags & (MPOL_F_STATIC_NODES | MPOL_F_RELATIVE_NODES))) {
            return std::unexpected(std::make_error_code(std::errc::invalid_argument));
        }
        for (const unsigned node : nodes) {
            if (node >= kMaxNodes) {
                return std::unexpected(std::make_error_code(std::errc::invalid_argument));
            }
        }
        NodeMask mask{};
        for (const unsigned node : nodes) {
            mask[node / kWordBits] |= 1UL << (node % kWordBits);
        }
        if (syscall(SYS_set_mempolicy, nativeMode,
                    nodes.empty() ? nullptr : mask.data(),
                    // Linux get_nodes() decrements maxnode before copying and
                    // clears the unused final bits; retain ID kMaxNodes - 1.
                    nodes.empty() ? 0UL : static_cast<unsigned long>(kMaxNodes) + 1UL) != 0) {
            return std::unexpected(std::error_code(errno, std::generic_category()));
        }
        return {};
#else
        (void)nativeMode;
        (void)nodes;
        return std::unexpected(std::make_error_code(std::errc::operation_not_supported));
#endif
    }

    static constexpr unsigned kWordBits = sizeof(unsigned long) * 8;
    using NodeMask = std::array<unsigned long, kMaxNodes / kWordBits>;

    static std::vector<unsigned> nodesFromMask(const NodeMask& mask)
    {
        std::vector<unsigned> nodes;
        for (unsigned node = 0; node < kMaxNodes; ++node) {
            if ((mask[node / kWordBits] & (1UL << (node % kWordBits))) != 0) {
                nodes.push_back(node);
            }
        }
        return nodes;
    }
};

} // namespace galay::utils

#endif // GALAY_UTILS_SYSTEM_MEMORY_HPP
