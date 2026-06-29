/**
 * @file balancer.hpp
 * @brief 负载均衡策略实现
 * @author galay-kernel
 * @version 1.0.0
 *
 * @details 提供多种负载均衡算法：
 * - RoundRobinLoadBalancer：轮询（原子计数器，天然线程安全）
 * - WeightRoundRobinLoadBalancer：平滑加权轮询
 * - RandomLoadBalancer：均匀随机选择
 * - WeightedRandomLoadBalancer：加权随机选择
 *
 * 所有负载均衡器暴露 select() 方法返回下一个节点。
 * RoundRobinLoadBalancer 使用原子操作，是线程安全的。
 * 其他均衡器不是线程安全的；需要外部同步。
 */

#ifndef GALAY_KERNEL_COMMON_BALANCER_HPP
#define GALAY_KERNEL_COMMON_BALANCER_HPP

#include <atomic>
#include <vector>
#include <memory>
#include <random>
#include <optional>
#include <concepts>

namespace galay::details
{

/**
 * @brief 轮询负载均衡器
 * @tparam Type 节点类型（必须可拷贝构造）
 * @details 使用原子计数器实现无锁线程安全选择
 */
template<std::copy_constructible Type>
class RoundRobinLoadBalancer
{
private:
    std::atomic<uint32_t> m_index;
    std::vector<Type> m_nodes;

public:
    using value_type = Type;
    using uptr = std::unique_ptr<RoundRobinLoadBalancer>;
    using ptr = std::shared_ptr<RoundRobinLoadBalancer>;

    RoundRobinLoadBalancer() : m_index(0) {}

    /**
     * @brief 以初始节点集合构造
     * @param nodes 节点列表
     */
    explicit RoundRobinLoadBalancer(const std::vector<Type>& nodes)
        : m_index(0), m_nodes(nodes) {}

    /**
     * @brief 以轮询顺序选择下一个节点（线程安全，无锁）
     * @return 被选中的节点，若无可用节点则返回 std::nullopt
     */
    std::optional<Type> select() {
        if (m_nodes.empty()) {
            return std::nullopt;
        }
        const uint32_t idx = m_index.fetch_add(1, std::memory_order_relaxed);
        return m_nodes[idx % m_nodes.size()];
    }

    /**
     * @brief 获取已注册的节点数
     * @return 节点数量
     */
    size_t size() const { return m_nodes.size(); }

    /**
     * @brief 向均衡器追加节点
     * @param node 要添加的节点
     */
    void append(Type node) {
        m_nodes.emplace_back(std::move(node));
    }
};

/**
 * @brief 平滑加权轮询负载均衡器
 * @tparam Type 节点类型（必须可拷贝构造）
 *
 * @details 实现 Nginx 风格的平滑加权轮询算法。
 * 非线程安全；需要外部同步。
 */
template<std::copy_constructible Type>
class WeightRoundRobinLoadBalancer
{
private:
    struct alignas(64) Node {
        Type node;
        int32_t current_weight;
        const int32_t fixed_weight;

        Node(Type n, int32_t weight)
            : node(std::move(n)), current_weight(0), fixed_weight(weight) {}
    };

    std::vector<Node> m_nodes;
    int32_t m_total_weight;

public:
    using value_type = Type;
    using uptr = std::unique_ptr<WeightRoundRobinLoadBalancer>;
    using ptr = std::shared_ptr<WeightRoundRobinLoadBalancer>;

    WeightRoundRobinLoadBalancer() : m_total_weight(0) {}

    /**
     * @brief 以节点和对应权重构造
     * @param nodes 节点列表
     * @param weights 各节点权重；若大小与 nodes 不匹配，所有权重默认为 1
     */
    WeightRoundRobinLoadBalancer(const std::vector<Type>& nodes, const std::vector<uint32_t>& weights)
        : m_total_weight(0)
    {
        if (nodes.size() != weights.size()) {
            m_nodes.reserve(nodes.size());
            for (const auto& n : nodes) {
                m_nodes.emplace_back(n, 1);
                m_total_weight += 1;
            }
        } else {
            m_nodes.reserve(nodes.size());
            for (size_t i = 0; i < nodes.size(); ++i) {
                m_nodes.emplace_back(nodes[i], static_cast<int32_t>(weights[i]));
                m_total_weight += static_cast<int32_t>(weights[i]);
            }
        }
    }

    /**
     * @brief 使用平滑加权轮询选择下一个节点（非线程安全）
     * @return 被选中的节点，若无可用节点则返回 std::nullopt
     * @note 仅在单线程上下文或使用外部锁时使用
     */
    std::optional<Type> select() {
        if (m_nodes.empty()) {
            return std::nullopt;
        }

        Node* selected = nullptr;
        for (auto& n : m_nodes) {
            n.current_weight += n.fixed_weight;
            if (selected == nullptr || n.current_weight > selected->current_weight) {
                selected = &n;
            }
        }

        if (selected != nullptr) {
            selected->current_weight -= m_total_weight;
            return selected->node;
        }
        return std::nullopt;
    }

    /**
     * @brief 获取已注册的节点数
     * @return 节点数量
     */
    size_t size() const { return m_nodes.size(); }

    /**
     * @brief 向均衡器追加带权重的节点
     * @param node 要添加的节点
     * @param weight 相对权重
     */
    void append(Type node, uint32_t weight) {
        m_nodes.emplace_back(std::move(node), static_cast<int32_t>(weight));
        m_total_weight += static_cast<int32_t>(weight);
    }
};

/**
 * @brief 均匀随机负载均衡器
 * @tparam Type 节点类型（必须可拷贝构造）
 *
 * @details 使用 mt19937_64 均匀随机选择节点。
 * 非线程安全；需要外部同步。
 */
template<std::copy_constructible Type>
class RandomLoadBalancer
{
private:
    std::vector<Type> m_nodes;
    std::mt19937_64 m_rng;

public:
    using value_type = Type;
    using uptr = std::unique_ptr<RandomLoadBalancer>;
    using ptr = std::shared_ptr<RandomLoadBalancer>;

    RandomLoadBalancer() {
        std::random_device rd;
        m_rng.seed(rd());
    }

    /**
     * @brief 以初始节点集合构造
     * @param nodes 节点列表
     */
    explicit RandomLoadBalancer(const std::vector<Type>& nodes) {
        std::random_device rd;
        m_rng.seed(rd());
        m_nodes = nodes;
    }

    /**
     * @brief 均匀随机选择节点（非线程安全）
     * @return 被选中的节点，若无可用节点则返回 std::nullopt
     * @note 仅在单线程上下文或使用外部锁时使用
     */
    std::optional<Type> select() {
        if (m_nodes.empty()) {
            return std::nullopt;
        }
        std::uniform_int_distribution<size_t> dist(0, m_nodes.size() - 1);
        return m_nodes[dist(m_rng)];
    }

    /**
     * @brief 获取已注册的节点数
     * @return 节点数量
     */
    size_t size() const { return m_nodes.size(); }

    /**
     * @brief 向均衡器追加节点
     * @param node 要添加的节点
     */
    void append(Type node) {
        m_nodes.emplace_back(std::move(node));
    }
};

/**
 * @brief 加权随机负载均衡器
 * @tparam Type 节点类型（必须可拷贝构造）
 *
 * @details 以与权重成正比的概率选择节点。
 * 非线程安全；需要外部同步。
 */
template<std::copy_constructible Type>
class WeightedRandomLoadBalancer
{
private:
    struct Node {
        Type node;
        uint32_t weight;

        Node(Type n, uint32_t w) : node(std::move(n)), weight(w) {}
    };

    std::vector<Node> m_nodes;
    uint32_t m_total_weight;
    std::mt19937 m_rng;

public:
    using value_type = Type;
    using uptr = std::unique_ptr<WeightedRandomLoadBalancer>;
    using ptr = std::shared_ptr<WeightedRandomLoadBalancer>;

    WeightedRandomLoadBalancer() : m_total_weight(0) {
        std::random_device rd;
        m_rng.seed(rd());
    }

    /**
     * @brief 以节点和对应权重构造
     * @param nodes 节点列表
     * @param weights 各节点权重；若大小与 nodes 不匹配，所有权重默认为 1
     */
    WeightedRandomLoadBalancer(const std::vector<Type>& nodes, const std::vector<uint32_t>& weights)
        : m_total_weight(0)
    {
        std::random_device rd;
        m_rng.seed(rd());

        if (nodes.size() != weights.size()) {
            m_nodes.reserve(nodes.size());
            for (const auto& n : nodes) {
                m_nodes.emplace_back(n, 1);
                m_total_weight += 1;
            }
        } else {
            m_nodes.reserve(nodes.size());
            for (size_t i = 0; i < nodes.size(); ++i) {
                m_nodes.emplace_back(nodes[i], weights[i]);
                m_total_weight += weights[i];
            }
        }
    }

    /**
     * @brief 以与权重成正比的概率选择节点（非线程安全）
     * @return 被选中的节点，若无节点或总权重为 0 则返回 std::nullopt
     * @note 仅在单线程上下文或使用外部锁时使用
     */
    std::optional<Type> select() {
        if (m_nodes.empty() || m_total_weight == 0) {
            return std::nullopt;
        }

        std::uniform_int_distribution<uint32_t> dist(1, m_total_weight);
        uint32_t random_weight = dist(m_rng);

        for (const auto& n : m_nodes) {
            if (random_weight <= n.weight) {
                return n.node;
            }
            random_weight -= n.weight;
        }
        return m_nodes.back().node;
    }

    /**
     * @brief 获取已注册的节点数
     * @return 节点数量
     */
    size_t size() const { return m_nodes.size(); }

    /**
     * @brief 向均衡器追加带权重的节点
     * @param node 要添加的节点
     * @param weight 相对权重
     */
    void append(Type node, uint32_t weight) {
        m_nodes.emplace_back(std::move(node), weight);
        m_total_weight += weight;
    }
};

} // namespace galay::details

#endif // GALAY_KERNEL_COMMON_BALANCER_HPP
