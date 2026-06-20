/**
 * @file balancer.hpp
 * @brief 负载均衡器集合
 * @author galay-utils
 * @version 1.0.0
 *
 * @details 提供四种负载均衡策略：轮询、加权轮询、随机、加权随机。
 *          轮询和随机版本是线程安全的，加权轮询为非线程安全。
 */

#ifndef GALAY_LOADBALANCER_HPP
#define GALAY_LOADBALANCER_HPP

#include "../common/defn.hpp"
#include <atomic>
#include <vector>
#include <memory>
#include <random>
#include <optional>

namespace galay::utils
{

/**
 * @brief 轮询负载均衡器（线程安全）
 * @details 按顺序依次选择节点，使用原子索引保证线程安全。
 * @tparam Type 节点类型
 */
template<typename Type>
class RoundRobinLoadBalancer
{
public:
    using value_type = Type; ///< 节点值类型
    using uptr = std::unique_ptr<RoundRobinLoadBalancer>; ///< 独占指针类型
    using ptr = std::shared_ptr<RoundRobinLoadBalancer>; ///< 共享指针类型

    /**
     * @brief 构造轮询负载均衡器
     * @param nodes 节点列表
     */
    explicit RoundRobinLoadBalancer(const std::vector<Type>& nodes)
        : m_index(0), m_nodes(nodes) {}

    /**
     * @brief 移动构造轮询负载均衡器
     * @param nodes 节点列表（右值）
     */
    explicit RoundRobinLoadBalancer(std::vector<Type>&& nodes)
        : m_index(0), m_nodes(std::move(nodes)) {}

    /**
     * @brief 选择下一个节点
     * @return 选中的节点，无节点时返回 std::nullopt
     */
    std::optional<Type> select();

    /**
     * @brief 获取节点数量
     * @return 节点数量
     */
    size_t size() const;

    /**
     * @brief 追加节点
     * @param node 新节点
     */
    void append(Type node);

private:
    std::atomic_uint32_t m_index;
    std::vector<Type> m_nodes;
};

/**
 * @brief 加权轮询负载均衡器（非线程安全），散列均匀
 * @details 使用平滑加权轮询算法（SWRR），节点使用缓存行对齐避免伪共享。
 * @tparam Type 节点类型
 */
template<typename Type>
class WeightRoundRobinLoadBalancer
{
    struct alignas(64) Node { ///< 缓存行对齐，避免伪共享
        Type node; ///< 节点值
        int32_t current_weight; ///< 当前动态权重
        const uint32_t fixed_weight; ///< 固定配置权重

        Node(Type n, uint32_t weight)
            : node(std::move(n)), current_weight(0), fixed_weight(weight) {}
    };

public:
    using value_type = Type; ///< 节点值类型
    using uptr = std::unique_ptr<WeightRoundRobinLoadBalancer>; ///< 独占指针类型
    using ptr = std::shared_ptr<WeightRoundRobinLoadBalancer>; ///< 共享指针类型

    /**
     * @brief 构造加权轮询负载均衡器
     * @param nodes 节点列表
     * @param weights 权重列表
     */
    WeightRoundRobinLoadBalancer(const std::vector<Type>& nodes, const std::vector<uint32_t>& weights);

    /**
     * @brief 移动构造加权轮询负载均衡器
     * @param nodes 节点列表（右值）
     * @param weights 权重列表
     */
    WeightRoundRobinLoadBalancer(std::vector<Type>&& nodes, const std::vector<uint32_t>& weights);

    /**
     * @brief 选择下一个节点
     * @return 选中的节点，无节点时返回 std::nullopt
     */
    std::optional<Type> select();

    /**
     * @brief 获取节点数量
     * @return 节点数量
     */
    size_t size() const;

    /**
     * @brief 追加节点
     * @param node 新节点
     * @param weight 节点权重
     */
    void append(Type node, uint32_t weight);

private:
    std::vector<Node> m_nodes;
    size_t m_current_index = 0;
};

/**
 * @brief 随机负载均衡器（线程安全）
 * @details 使用 Mersenne Twister 随机数生成器随机选择节点。
 * @tparam Type 节点类型
 */
template<typename Type>
class RandomLoadBalancer
{
public:
    using value_type = Type; ///< 节点值类型
    using uptr = std::unique_ptr<RandomLoadBalancer>; ///< 独占指针类型
    using ptr = std::shared_ptr<RandomLoadBalancer>; ///< 共享指针类型

    /**
     * @brief 构造随机负载均衡器
     * @param nodes 节点列表
     */
    explicit RandomLoadBalancer(const std::vector<Type>& nodes);

    /**
     * @brief 移动构造随机负载均衡器
     * @param nodes 节点列表（右值）
     */
    explicit RandomLoadBalancer(std::vector<Type>&& nodes);

    /**
     * @brief 随机选择一个节点
     * @return 选中的节点，无节点时返回 std::nullopt
     */
    std::optional<Type> select();

    /**
     * @brief 获取节点数量
     * @return 节点数量
     */
    size_t size() const;

    /**
     * @brief 追加节点
     * @param node 新节点
     */
    void append(Type node);

private:
    std::vector<Type> m_nodes;
    std::mt19937_64 m_rng;
};

/**
 * @brief 加权随机负载均衡器（线程安全）
 * @details 按权重比例随机选择节点，权重越大被选中概率越高。
 * @tparam Type 节点类型
 */
template<typename Type>
class WeightedRandomLoadBalancer
{
    struct Node {
        Type node; ///< 节点值
        uint32_t weight; ///< 节点权重
    };

public:
    using value_type = Type; ///< 节点值类型
    using uptr = std::unique_ptr<WeightedRandomLoadBalancer>; ///< 独占指针类型
    using ptr = std::shared_ptr<WeightedRandomLoadBalancer>; ///< 共享指针类型

    /**
     * @brief 构造加权随机负载均衡器
     * @param nodes 节点列表
     * @param weights 权重列表
     */
    WeightedRandomLoadBalancer(const std::vector<Type>& nodes, const std::vector<uint32_t>& weights);

    /**
     * @brief 移动构造加权随机负载均衡器
     * @param nodes 节点列表（右值）
     * @param weights 权重列表
     */
    WeightedRandomLoadBalancer(std::vector<Type>&& nodes, const std::vector<uint32_t>& weights);

    /**
     * @brief 按权重随机选择一个节点
     * @return 选中的节点，无节点时返回 std::nullopt
     */
    std::optional<Type> select();

    /**
     * @brief 获取节点数量
     * @return 节点数量
     */
    size_t size() const;

    /**
     * @brief 追加节点
     * @param node 新节点
     * @param weight 节点权重
     */
    void append(Type node, uint32_t weight);

private:
    std::vector<Node> m_nodes;
    uint32_t m_total_weight = 0;
    std::mt19937 m_rng;
};

} // namespace galay::utils

#include "balancer.inl"

#endif // GALAY_LOADBALANCER_HPP
