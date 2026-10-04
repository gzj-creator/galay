#ifndef GALAY_UTILS_SYSTEM_NUMA_HPP
#define GALAY_UTILS_SYSTEM_NUMA_HPP

#include <array>
#include <cerrno>
#include <expected>
#include <system_error>
#include <vector>

#if defined(__linux__)
#include <linux/mempolicy.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace galay::utils {

/**
 * @brief NUMA 拓扑约束查询。
 * @details 该类只查询当前线程所属 cpuset 允许使用的内存节点；内存策略
 *          的读取和设置由 Memory 提供。
 */
class Numa {
public:
    static constexpr unsigned kMaxNodes = 1024;

    /** @brief 读取当前线程所属 cpuset 允许使用的内存节点。 */
    [[nodiscard]] static std::expected<std::vector<unsigned>, std::error_code>
    allowedNumaNodes()
    {
#if defined(__linux__)
        NodeMask mask{};
        if (syscall(SYS_get_mempolicy, nullptr, mask.data(),
                    static_cast<unsigned long>(kMaxNodes), nullptr,
                    static_cast<unsigned long>(MPOL_F_MEMS_ALLOWED)) != 0) {
            return std::unexpected(std::error_code(errno, std::generic_category()));
        }
        return nodesFromMask(mask);
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

#endif // GALAY_UTILS_SYSTEM_NUMA_HPP
