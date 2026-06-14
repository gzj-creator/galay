#ifndef GALAY_LOADBALANCER_INL
#define GALAY_LOADBALANCER_INL

#include "balancer.hpp"

namespace galay::utils
{

template<typename Type>
inline std::optional<Type> RoundRobinLoadBalancer<Type>::select()
{
    if (m_nodes.empty())
        return std::nullopt;
    const uint32_t idx = m_index.fetch_add(1, std::memory_order_relaxed);
    return m_nodes[idx % m_nodes.size()];
}

template<typename Type>
inline size_t RoundRobinLoadBalancer<Type>::size() const
{
    return m_nodes.size();
}

template<typename Type>
inline void RoundRobinLoadBalancer<Type>::append(Type node)
{
    m_nodes.emplace_back(std::move(node));
}

template<typename Type>
inline WeightRoundRobinLoadBalancer<Type>::WeightRoundRobinLoadBalancer(const std::vector<Type>& nodes, const std::vector<uint32_t>& weights)
{
    if (nodes.size() != weights.size()) {
        throw std::invalid_argument("nodes and weights size mismatch");
    }
    m_nodes.reserve(nodes.size());
    for(size_t i = 0; i < nodes.size(); ++i) {
        m_nodes.emplace_back(nodes[i], weights[i]);
    }
    m_current_index = 0;
}

template<typename Type>
inline WeightRoundRobinLoadBalancer<Type>::WeightRoundRobinLoadBalancer(std::vector<Type>&& nodes, const std::vector<uint32_t>& weights)
{
    if (nodes.size() != weights.size()) {
        throw std::invalid_argument("nodes and weights size mismatch");
    }
    m_nodes.reserve(nodes.size());
    for(size_t i = 0; i < nodes.size(); ++i) {
        m_nodes.emplace_back(std::move(nodes[i]), weights[i]);
    }
    m_current_index = 0;
}

template<typename Type>
inline std::optional<Type> WeightRoundRobinLoadBalancer<Type>::select()
{
    if(m_nodes.empty()) {
        return std::nullopt;
    }

    // 找到当前权重最大的节点
    Node* selected = nullptr;
    int32_t max_weight = std::numeric_limits<int32_t>::min();

    for (auto& node : m_nodes) {
        node.current_weight += node.fixed_weight;
        if (node.current_weight > max_weight) {
            max_weight = node.current_weight;
            selected = &node;
        }
    }

    if (!selected) {
        return std::nullopt;
    }

    // 减少选中节点的权重 - 减去所有节点的固定权重总和
    int32_t total_weight = 0;
    for (const auto& node : m_nodes) {
        total_weight += node.fixed_weight;
    }
    selected->current_weight -= total_weight;

    return selected->node;
}

template<typename Type>
inline size_t WeightRoundRobinLoadBalancer<Type>::size() const
{
    return m_nodes.size();
}

template<typename Type>
inline void WeightRoundRobinLoadBalancer<Type>::append(Type node, uint32_t weight)
{
    m_nodes.emplace_back(std::move(node), weight);
}

template<typename Type>
inline RandomLoadBalancer<Type>::RandomLoadBalancer(const std::vector<Type>& nodes)
    : m_nodes(nodes)
{
    std::random_device rd;
    m_rng.seed(rd());
}

template<typename Type>
inline RandomLoadBalancer<Type>::RandomLoadBalancer(std::vector<Type>&& nodes)
    : m_nodes(std::move(nodes))
{
    std::random_device rd;
    m_rng.seed(rd());
}

template<typename Type>
inline std::optional<Type> RandomLoadBalancer<Type>::select()
{
    if (m_nodes.empty()) {
        return std::nullopt;
    }
    std::uniform_int_distribution<size_t> dist(0, m_nodes.size() - 1);
    return m_nodes[dist(m_rng)];
}

template<typename Type>
inline size_t RandomLoadBalancer<Type>::size() const
{
    return m_nodes.size();
}

template<typename Type>
inline void RandomLoadBalancer<Type>::append(Type node)
{
    m_nodes.emplace_back(std::move(node));
}

template<typename Type>
inline WeightedRandomLoadBalancer<Type>::WeightedRandomLoadBalancer(const std::vector<Type>& nodes, const std::vector<uint32_t>& weights)
{
    if (nodes.size() != weights.size()) {
        throw std::invalid_argument("nodes and weights size mismatch");
    }
    m_nodes.reserve(nodes.size());
    std::random_device rd;
    m_rng.seed(rd());

    for(size_t i = 0; i < nodes.size(); ++i) {
        m_nodes.emplace_back(Node{nodes[i], weights[i]});
        m_total_weight += weights[i];
    }
}

template<typename Type>
inline WeightedRandomLoadBalancer<Type>::WeightedRandomLoadBalancer(std::vector<Type>&& nodes, const std::vector<uint32_t>& weights)
{
    if (nodes.size() != weights.size()) {
        throw std::invalid_argument("nodes and weights size mismatch");
    }
    m_nodes.reserve(nodes.size());
    std::random_device rd;
    m_rng.seed(rd());

    for(size_t i = 0; i < nodes.size(); ++i) {
        m_nodes.emplace_back(Node{std::move(nodes[i]), weights[i]});
        m_total_weight += weights[i];
    }
}

template<typename Type>
inline std::optional<Type> WeightedRandomLoadBalancer<Type>::select()
{
    if (m_nodes.empty() || m_total_weight == 0) {
        return std::nullopt;
    }

    uint32_t random_weight = std::uniform_int_distribution<uint32_t>(0, m_total_weight - 1)(m_rng);
    uint32_t current_weight = 0;

    for(const auto& node : m_nodes) {
        current_weight += node.weight;
        if(random_weight < current_weight) {
            return node.node;
        }
    }

    return std::nullopt; // 理论上不会到达这里
}

template<typename Type>
inline size_t WeightedRandomLoadBalancer<Type>::size() const
{
    return m_nodes.size();
}

template<typename Type>
inline void WeightedRandomLoadBalancer<Type>::append(Type node, uint32_t weight)
{
    m_nodes.emplace_back(Node{std::move(node), weight});
    m_total_weight += weight;
}

} // namespace galay::utils

#endif // GALAY_LOADBALANCER_INL
