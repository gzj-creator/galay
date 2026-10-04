#ifndef GALAY_UTILS_SYSTEM_MEMORY_HPP
#define GALAY_UTILS_SYSTEM_MEMORY_HPP

#include "numa.hpp"

#include <array>
#include <cerrno>
#include <expected>
#include <span>
#include <system_error>
#include <vector>

#if defined(__linux__)
#include <linux/mempolicy.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace galay::utils {

/**
 * @brief 当前线程后续内存分配使用的默认 NUMA 内存策略。
 * @details 策略只影响后续新页的分配和首次触碰，不迁移已经存在的页；新建
 *          Linux 线程继承创建者当时的策略。
 */
class Memory {
public:
    static constexpr unsigned kMaxNodes = Numa::kMaxNodes;

    enum class Policy { Default = 0, Bind = 2, Interleave = 3 };

    struct PolicyState {
        // 保留 Linux 原生 mode，包括本接口没有设置的 flags。
        int native_mode = 0;
        std::vector<unsigned> nodes;
    };

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
        if ((policy != Policy::Default && policy != Policy::Bind &&
             policy != Policy::Interleave) ||
            (policy == Policy::Default ? !nodes.empty() : nodes.empty())) {
            return std::unexpected(std::make_error_code(std::errc::invalid_argument));
        }
        for (const unsigned node : nodes) {
            if (node >= kMaxNodes) {
                return std::unexpected(std::make_error_code(std::errc::invalid_argument));
            }
        }
#if defined(__linux__)
        NodeMask mask{};
        for (const unsigned node : nodes) {
            mask[node / kWordBits] |= 1UL << (node % kWordBits);
        }
        if (syscall(SYS_set_mempolicy, static_cast<int>(policy),
                    nodes.empty() ? nullptr : mask.data(),
                    nodes.empty() ? 0UL : static_cast<unsigned long>(kMaxNodes)) != 0) {
            return std::unexpected(std::error_code(errno, std::generic_category()));
        }
        return {};
#else
        return std::unexpected(std::make_error_code(std::errc::operation_not_supported));
#endif
    }

private:
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
