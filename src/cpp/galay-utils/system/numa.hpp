#ifndef GALAY_UTILS_SYSTEM_NUMA_HPP
#define GALAY_UTILS_SYSTEM_NUMA_HPP

#include "detail/sysfs.hpp"

#include <array>
#include <cerrno>
#include <expected>
#include <limits>
#include <system_error>
#include <utility>
#include <vector>

#if defined(__linux__)
#include <linux/mempolicy.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace galay::utils {

namespace detail {
inline constexpr unsigned kMaxNumaNodes = 1024;
}

#if defined(__linux__)
namespace detail {

// Internal sysfs reader. An explicit root lets tests exercise missing files,
// sparse node IDs and CPU-less nodes without changing the host's sysfs.
class NumaTopology {
public:
    static constexpr unsigned kMaxNodes = kMaxNumaNodes;

    explicit NumaTopology(std::string root = "/sys/devices/system/node")
        : m_root(std::move(root)) {}

    [[nodiscard]] std::expected<std::vector<unsigned>, std::error_code> online_nodes() const
    {
        return read_system_ids(m_root + "/online", kMaxNodes);
    }

    [[nodiscard]] std::expected<std::vector<unsigned>, std::error_code>
    cpus_of_node(unsigned node) const
    {
        if (node >= kMaxNodes) {
            return std::unexpected(std::make_error_code(std::errc::invalid_argument));
        }
        const auto online = online_nodes();
        if (!online) {
            return std::unexpected(online.error());
        }
        if (!std::binary_search(online->begin(), online->end(), node)) {
            return std::unexpected(std::make_error_code(std::errc::invalid_argument));
        }
        return read_node_cpus(node);
    }

    [[nodiscard]] std::expected<unsigned, std::error_code> node_of_cpu(unsigned cpu) const
    {
        if (cpu >= static_cast<unsigned>(std::numeric_limits<int>::max())) {
            return std::unexpected(std::make_error_code(std::errc::invalid_argument));
        }
        const auto online = online_nodes();
        if (!online) {
            return std::unexpected(online.error());
        }
        for (const unsigned node : *online) {
            const auto cpus = read_node_cpus(node);
            if (!cpus) {
                return std::unexpected(cpus.error());
            }
            if (std::binary_search(cpus->begin(), cpus->end(), cpu)) {
                return node;
            }
        }
        return std::unexpected(std::make_error_code(std::errc::no_such_device));
    }

    [[nodiscard]] std::expected<unsigned, std::error_code>
    distance(unsigned from, unsigned to) const
    {
        if (from >= kMaxNodes || to >= kMaxNodes) {
            return std::unexpected(std::make_error_code(std::errc::invalid_argument));
        }
        const auto online = online_nodes();
        if (!online) {
            return std::unexpected(online.error());
        }
        const auto target = std::lower_bound(online->begin(), online->end(), to);
        if (!std::binary_search(online->begin(), online->end(), from) ||
            target == online->end() || *target != to) {
            return std::unexpected(std::make_error_code(std::errc::invalid_argument));
        }
        const auto file = read_system_text(node_path(from) + "/distance");
        if (!file) {
            return std::unexpected(file.error());
        }
        auto text = trim_system_text(*file);
        std::vector<unsigned> distances;
        while (!text.empty()) {
            const auto end = text.find_first_of(" \t\n\r\f\v");
            const auto token = text.substr(0, end);
            unsigned value = 0;
            const auto parsed = std::from_chars(token.data(), token.data() + token.size(), value);
            if (parsed.ec != std::errc{}) {
                return std::unexpected(std::make_error_code(parsed.ec));
            }
            if (parsed.ptr != token.data() + token.size() || value == 0) {
                return std::unexpected(std::make_error_code(std::errc::invalid_argument));
            }
            distances.push_back(value);
            text = end == std::string_view::npos ? std::string_view{}
                : trim_system_text(text.substr(end));
        }
        // Linux lists distance columns in online-node order, not at column
        // `node ID`. This matters when online nodes are e.g. {0, 2}.
        if (distances.size() != online->size()) {
            return std::unexpected(std::make_error_code(std::errc::invalid_argument));
        }
        return distances[static_cast<std::size_t>(target - online->begin())];
    }

private:
    std::string node_path(unsigned node) const
    {
        return m_root + "/node" + std::to_string(node);
    }

    [[nodiscard]] std::expected<std::vector<unsigned>, std::error_code>
    read_node_cpus(unsigned node) const
    {
        return read_system_ids(node_path(node) + "/cpulist",
                             std::numeric_limits<int>::max(), true);
    }

    std::string m_root;
};

} // namespace detail
#endif

/**
 * @brief 系统 NUMA 拓扑和当前线程所属 cpuset 的内存节点约束。
 * @details 在线节点、CPU/节点关系和相对距离来自 Linux sysfs，属于系统
 *          拓扑快照，不按 cpuset 过滤；内存策略的读取、设置和恢复由 Memory 提供。
 */
class Numa {
public:
    static constexpr unsigned kMaxNodes = detail::kMaxNumaNodes;

    /** @brief 读取系统在线 NUMA 节点快照，不等同于当前 cpuset 允许节点。 */
    [[nodiscard]] static std::expected<std::vector<unsigned>, std::error_code> online_nodes()
    {
#if defined(__linux__)
        return detail::NumaTopology{}.online_nodes();
#else
        return std::unexpected(std::make_error_code(std::errc::operation_not_supported));
#endif
    }

    /** @brief 返回 sysfs 拓扑中 CPU 所属的在线节点；未找到 CPU 返回 no_such_device。 */
    [[nodiscard]] static std::expected<unsigned, std::error_code> node_of_cpu(unsigned cpu)
    {
#if defined(__linux__)
        return detail::NumaTopology{}.node_of_cpu(cpu);
#else
        (void)cpu;
        return std::unexpected(std::make_error_code(std::errc::operation_not_supported));
#endif
    }

    /** @brief 读取在线节点的在线 CPU 列表；不按 affinity 过滤，无 CPU 的节点返回空集。 */
    [[nodiscard]] static std::expected<std::vector<unsigned>, std::error_code>
    cpus_of_node(unsigned node)
    {
#if defined(__linux__)
        return detail::NumaTopology{}.cpus_of_node(node);
#else
        (void)node;
        return std::unexpected(std::make_error_code(std::errc::operation_not_supported));
#endif
    }

    /** @brief 返回 sysfs 的正整数相对距离，无时间/字节单位。 */
    [[nodiscard]] static std::expected<unsigned, std::error_code>
    distance(unsigned from, unsigned to)
    {
#if defined(__linux__)
        return detail::NumaTopology{}.distance(from, to);
#else
        (void)from;
        (void)to;
        return std::unexpected(std::make_error_code(std::errc::operation_not_supported));
#endif
    }

    /** @brief 读取当前线程所属 cpuset 允许使用的内存节点。 */
    [[nodiscard]] static std::expected<std::vector<unsigned>, std::error_code>
    allowed_numa_nodes()
    {
#if defined(__linux__)
        NodeMask mask{};
        if (syscall(SYS_get_mempolicy, nullptr, mask.data(),
                    static_cast<unsigned long>(kMaxNodes), nullptr,
                    static_cast<unsigned long>(MPOL_F_MEMS_ALLOWED)) != 0) {
            return std::unexpected(std::error_code(errno, std::generic_category()));
        }
        return nodes_from_mask(mask);
#else
        return std::unexpected(std::make_error_code(std::errc::operation_not_supported));
#endif
    }

private:
    static constexpr unsigned kWordBits = sizeof(unsigned long) * 8;
    using NodeMask = std::array<unsigned long, kMaxNodes / kWordBits>;

    static std::vector<unsigned> nodes_from_mask(const NodeMask& mask)
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
