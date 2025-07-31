#ifndef GALAY_STRATEGY_INL
#define GALAY_STRATEGY_INL

#include "Strategy.hpp"

namespace galay::details
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
inline WeightRoundRobinLoadBalancer<Type>::WeightRoundRobinLoadBalancer(const std::vector<Type>& nodes,const std::vector<uint32_t>& weights)
{
    m_index.store(0);
    m_nodes.reserve(nodes.size());
    for(size_t i = 0; i < nodes.size(); ++i) {
        m_nodes[i] = {std::move(nodes[i]), weights[i], 0};
    }
}

template<typename Type>
inline std::optional<Type> WeightRoundRobinLoadBalancer<Type>::select() 
{
    if(m_nodes.empty()) {
        return std::nullopt;
    }
    uint32_t idx = m_index.load(std::memory_order_relaxed);
    Node* selected = nullptr;
    do {
        Node& node = m_nodes[idx % m_nodes.size()];
        node.current_weight += node.fixed_weight;
        
        if (!selected || node.current_weight > selected->current_weight) {
            selected = &node;
        }
        
        idx = (idx + 1) % m_nodes.size();
    } while (idx != m_index.load(std::memory_order_relaxed));
    
    selected->current_weight -= selected->fixed_weight;
    m_index.store(idx, std::memory_order_relaxed);
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
    m_nodes.emplace_back(std::move(node), weight, 0);
}

template<typename Type>
inline RandomLoadBalancer<Type>::RandomLoadBalancer(const std::vector<Type>& nodes)
    : m_nodes(nodes) 
{
    static std::atomic_flag init_flag = ATOMIC_FLAG_INIT;
    if (!init_flag.test_and_set()) {
        std::random_device rd;
        m_rng.seed(rd());
    }
}

template<typename Type>
inline std::optional<Type> RandomLoadBalancer<Type>::select()
{
    if (m_nodes.empty()) {
        return std::nullopt;
    }
    return m_nodes[std::uniform_int_distribution<size_t>(0, m_nodes.size() - 1)(m_rng)];
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
    m_nodes.reserve(nodes.size());
    m_random = std::mt19937(std::random_device()());
    for(size_t i = 0; i < nodes.size(); ++i) {
        m_nodes[i] = {std::move(nodes[i]), weights[i], 0};
        m_total_weight += weights[i];
    }
}

template<typename Type>
inline std::optional<Type> WeightedRandomLoadBalancer<Type>::select()
{
    uint32_t random_weight = std::uniform_int_distribution<uint32_t>(0, m_total_weight)(m_random);
    for(size_t i = 0; i < m_nodes.size(); ++i) {
        random_weight -= m_nodes[i].weight;
        if(random_weight <= 0) return m_nodes[i].node;
    }
    return std::nullopt;
}

template<typename Type>
inline size_t WeightedRandomLoadBalancer<Type>::size() const
{
    return m_nodes.size();
}

template<typename Type>
inline void WeightedRandomLoadBalancer<Type>::append(Type node, uint32_t weight)
{
    m_nodes.emplace_back(std::move(node), weight);
    m_total_weight += weight;
}


}

#endif