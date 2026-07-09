/**
 * @file hash.hpp
 * @brief 一致性哈希实现
 * @author galay-utils
 * @version 1.0.0
 *
 * @details 提供带虚拟节点的一致性哈希环，支持节点动态添加/移除、
 *          健康检查、加权节点和多副本查询。使用原子快照发布避免阻塞调度线程。
 */

#ifndef GALAY_UTILS_CONSISTENT_HASH_HPP
#define GALAY_UTILS_CONSISTENT_HASH_HPP

#include "../common/defn.hpp"
#include <string>
#include <map>
#include <set>
#include <vector>
#include <functional>
#include <optional>
#include <atomic>
#include <limits>
#include <memory>
#include <unordered_map>

namespace galay::utils {

/**
 * @brief MurmurHash3 内部哈希实现
 * @details 用于一致性哈希环的哈希计算。
 */
class MurmurHash3 {
public:
    /**
     * @brief 计算 32 位 MurmurHash3 哈希值
     * @param key 输入数据指针
     * @param len 数据长度
     * @param seed 哈希种子值
     * @return 32 位哈希值
     */
    static uint32_t hash32(const void* key, size_t len, uint32_t seed = 0) {
        const uint8_t* data = static_cast<const uint8_t*>(key);
        const int nblocks = static_cast<int>(len / 4);

        uint32_t h1 = seed;
        const uint32_t c1 = 0xcc9e2d51;
        const uint32_t c2 = 0x1b873593;

        const uint32_t* blocks = reinterpret_cast<const uint32_t*>(data + nblocks * 4);

        for (int i = -nblocks; i; i++) {
            uint32_t k1 = blocks[i];

            k1 *= c1;
            k1 = rotl32(k1, 15);
            k1 *= c2;

            h1 ^= k1;
            h1 = rotl32(h1, 13);
            h1 = h1 * 5 + 0xe6546b64;
        }

        const uint8_t* tail = data + nblocks * 4;
        uint32_t k1 = 0;

        switch (len & 3) {
            case 3: k1 ^= tail[2] << 16; [[fallthrough]];
            case 2: k1 ^= tail[1] << 8;  [[fallthrough]];
            case 1: k1 ^= tail[0];
                    k1 *= c1;
                    k1 = rotl32(k1, 15);
                    k1 *= c2;
                    h1 ^= k1;
        }

        h1 ^= static_cast<uint32_t>(len);
        h1 = fmix32(h1);

        return h1;
    }

    /**
     * @brief 计算字符串的 32 位 MurmurHash3 哈希值
     * @param key 输入字符串
     * @param seed 哈希种子值
     * @return 32 位哈希值
     */
    static uint32_t hash32(const std::string& key, uint32_t seed = 0) {
        return hash32(key.data(), key.size(), seed);
    }

private:
    static uint32_t rotl32(uint32_t x, int8_t r) {
        return (x << r) | (x >> (32 - r));
    }

    static uint32_t fmix32(uint32_t h) {
        h ^= h >> 16;
        h *= 0x85ebca6b;
        h ^= h >> 13;
        h *= 0xc2b2ae35;
        h ^= h >> 16;
        return h;
    }
};

/**
 * @brief 节点状态结构体
 * @details 记录节点的健康状态、请求计数和失败计数，所有字段均为原子类型。
 */
struct NodeStatus {
    std::atomic<uint64_t> requestCount{0}; ///< 请求计数
    std::atomic<uint64_t> failureCount{0}; ///< 失败计数
    std::atomic<bool> healthy{true}; ///< 是否健康

    void recordRequest() {
        const uint64_t previous = requestCount.fetch_add(1, std::memory_order_relaxed);
        if (previous == std::numeric_limits<uint64_t>::max()) {
            requestCount.store(previous, std::memory_order_relaxed);
        }
    } ///< 记录一次请求

    void recordFailure() {
        const uint64_t previous = failureCount.fetch_add(1, std::memory_order_relaxed);
        if (previous == std::numeric_limits<uint64_t>::max()) {
            failureCount.store(previous, std::memory_order_relaxed);
        }
        healthy.store(false, std::memory_order_release);
    } ///< 记录一次失败并标记为不健康

    void markHealthy() { healthy.store(true, std::memory_order_release); } ///< 标记为健康

    void reset() {
        requestCount.store(0, std::memory_order_relaxed);
        failureCount.store(0, std::memory_order_relaxed);
        healthy.store(true, std::memory_order_release);
    } ///< 重置所有计数器
};

/**
 * @brief 节点配置结构体
 * @details 定义一致性哈希节点的标识、端点和权重。
 */
struct NodeConfig {
    std::string id; ///< 节点标识
    std::string endpoint; ///< 节点端点地址
    int weight = 1; ///< 节点权重

    bool operator==(const NodeConfig& other) const {
        return id == other.id;
    }
};

/**
 * @brief 物理节点结构体
 * @details 包含节点配置和运行时状态。
 */
struct PhysicalNode {
    NodeConfig config; ///< 节点配置
    NodeStatus status; ///< 节点状态

    explicit PhysicalNode(NodeConfig cfg) : config(std::move(cfg)) {}
};

/**
 * @brief 一致性哈希环
 * @details 基于虚拟节点的一致性哈希实现。拓扑通过 copy-on-write 快照更新，
 *          读路径只原子加载快照，不使用会阻塞协程调度线程的互斥锁。
 *          节点健康状态和计数使用原子字段，可在已发布快照中独立更新。
 */
class ConsistentHash {
    struct RingSnapshot {
        std::map<uint32_t, std::shared_ptr<PhysicalNode>> ring;
        std::unordered_map<std::string, std::shared_ptr<PhysicalNode>> nodes;
        RingSnapshot* next_retired = nullptr;
    };

    class SnapshotGuard {
    public:
        explicit SnapshotGuard(const ConsistentHash& owner)
            : m_owner(owner)
        {
            const size_t previous = m_owner.m_readers.fetch_add(1, std::memory_order_acquire);
            if (previous == std::numeric_limits<size_t>::max()) {
                m_owner.m_readers.store(previous, std::memory_order_release);
            }
            m_snapshot = m_owner.m_state.load(std::memory_order_acquire);
        }

        ~SnapshotGuard()
        {
            const size_t previous = m_owner.m_readers.fetch_sub(1, std::memory_order_release);
            if (previous == 0) {
                m_owner.m_readers.store(0, std::memory_order_release);
            }
        }

        SnapshotGuard(const SnapshotGuard&) = delete;
        SnapshotGuard& operator=(const SnapshotGuard&) = delete;

        const RingSnapshot* get() const { return m_snapshot; }

    private:
        const ConsistentHash& m_owner;
        const RingSnapshot* m_snapshot;
    };

public:
    /// 哈希函数类型
    using HashFunc = std::function<uint32_t(const std::string&)>;

    /**
     * @brief 构造一致性哈希环
     * @param virtualNodes 每个物理节点对应的虚拟节点数量（默认 150）
     * @param hashFunc 自定义哈希函数，为空时使用 MurmurHash3
     */
    explicit ConsistentHash(size_t virtualNodes = 150,
                            HashFunc hashFunc = nullptr)
        : m_virtualNodes(virtualNodes)
        , m_hashFunc(hashFunc ? std::move(hashFunc) :
                     [](const std::string& key) { return MurmurHash3::hash32(key); })
        , m_state(nullptr)
        , m_retired(nullptr)
        , m_readers(0)
    {
        auto initial = std::make_unique<RingSnapshot>();
        m_state.store(initial.release(), std::memory_order_release);
    }

    /**
     * @brief 释放由 copy-on-write 发布过的所有快照。
     * @note 析构要求外部已停止并发访问该对象；这与对象生命周期的一般规则一致。
     */
    ~ConsistentHash() {
        delete m_state.exchange(nullptr, std::memory_order_acq_rel);

        RingSnapshot* snapshot = m_retired.exchange(nullptr, std::memory_order_acq_rel);
        while (snapshot != nullptr) {
            RingSnapshot* next = snapshot->next_retired;
            delete snapshot;
            snapshot = next;
        }
    }

    ConsistentHash(const ConsistentHash&) = delete;
    ConsistentHash& operator=(const ConsistentHash&) = delete;
    ConsistentHash(ConsistentHash&&) = delete;
    ConsistentHash& operator=(ConsistentHash&&) = delete;

    /**
     * @brief 添加节点到哈希环
     * @param config 节点配置
     */
    void addNode(const NodeConfig& config) {
        auto node = std::make_shared<PhysicalNode>(config);
        const RingSnapshot* retired = nullptr;
        {
            SnapshotGuard guard(*this);
            const RingSnapshot* current = guard.get();
            while (true) {
                RingSnapshot* next = cloneSnapshot(*current);
                next->nodes[config.id] = node;

                size_t vnodes = m_virtualNodes * config.weight;
                for (size_t i = 0; i < vnodes; ++i) {
                    std::string virtualKey = config.id + "#" + std::to_string(i);
                    uint32_t hash = m_hashFunc(virtualKey);
                    next->ring[hash] = node;
                }

                const RingSnapshot* expected = current;
                if (m_state.compare_exchange_weak(
                        expected,
                        next,
                        std::memory_order_release,
                        std::memory_order_acquire)) {
                    retired = current;
                    break;
                }
                delete next;
                current = expected;
            }
        }
        retireSnapshot(retired);
    }

    /**
     * @brief 从哈希环移除节点
     * @param nodeId 节点标识
     */
    void removeNode(const std::string& nodeId) {
        const RingSnapshot* retired = nullptr;
        {
            SnapshotGuard guard(*this);
            const RingSnapshot* current = guard.get();
            while (true) {
                auto it = current->nodes.find(nodeId);
                if (it == current->nodes.end()) {
                    return;
                }

                RingSnapshot* next = cloneSnapshot(*current);
                const auto& config = it->second->config;
                size_t vnodes = m_virtualNodes * config.weight;

                for (size_t i = 0; i < vnodes; ++i) {
                    std::string virtualKey = nodeId + "#" + std::to_string(i);
                    uint32_t hash = m_hashFunc(virtualKey);
                    const size_t erased = next->ring.erase(hash);
                    if (erased == 0) {
                        continue;
                    }
                }

                const size_t erased_node = next->nodes.erase(nodeId);
                if (erased_node == 0) {
                    delete next;
                    return;
                }
                const RingSnapshot* expected = current;
                if (m_state.compare_exchange_weak(
                        expected,
                        next,
                        std::memory_order_release,
                        std::memory_order_acquire)) {
                    retired = current;
                    break;
                }
                delete next;
                current = expected;
            }
        }
        retireSnapshot(retired);
    }

    /**
     * @brief 根据 key 获取对应的节点
     * @param key 查找键
     * @return 节点配置，环为空时返回 std::nullopt
     */
    std::optional<NodeConfig> getNode(const std::string& key) const {
        SnapshotGuard guard(*this);
        const RingSnapshot* state = guard.get();

        if (state->ring.empty()) {
            return std::nullopt;
        }

        uint32_t hash = m_hashFunc(key);
        auto it = state->ring.lower_bound(hash);

        if (it == state->ring.end()) {
            it = state->ring.begin();
        }

        it->second->status.recordRequest();
        return it->second->config;
    }

    /**
     * @brief 获取健康的节点（跳过不健康节点）
     * @param key 查找键
     * @param maxRetries 最大重试次数（默认 3）
     * @return 健康的节点配置，未找到时返回 std::nullopt
     */
    std::optional<NodeConfig> getHealthyNode(const std::string& key, size_t maxRetries = 3) const {
        SnapshotGuard guard(*this);
        const RingSnapshot* state = guard.get();

        if (state->ring.empty()) {
            return std::nullopt;
        }

        uint32_t hash = m_hashFunc(key);
        auto it = state->ring.lower_bound(hash);

        for (size_t retry = 0; retry < maxRetries; ++retry) {
            if (it == state->ring.end()) {
                it = state->ring.begin();
            }

            if (it->second->status.healthy.load(std::memory_order_acquire)) {
                it->second->status.recordRequest();
                return it->second->config;
            }

            ++it;
        }

        return std::nullopt;
    }

    /**
     * @brief 获取多个不同的节点（用于多副本）
     * @param key 查找键
     * @param count 需要的节点数量
     * @return 节点配置列表
     */
    std::vector<NodeConfig> getNodes(const std::string& key, size_t count) const {
        SnapshotGuard guard(*this);
        const RingSnapshot* state = guard.get();

        std::vector<NodeConfig> result;
        if (state->ring.empty() || count == 0) {
            return result;
        }

        uint32_t hash = m_hashFunc(key);
        auto it = state->ring.lower_bound(hash);

        std::set<std::string> seen;
        size_t iterations = 0;
        size_t maxIterations = state->ring.size();

        while (result.size() < count && iterations < maxIterations) {
            if (it == state->ring.end()) {
                it = state->ring.begin();
            }

            const auto& nodeId = it->second->config.id;
            if (seen.find(nodeId) == seen.end()) {
                auto [seenIt, inserted] = seen.insert(nodeId);
                if (inserted && seenIt != seen.end()) {
                    result.push_back(it->second->config);
                }
            }

            ++it;
            ++iterations;
        }

        return result;
    }

    void markUnhealthy(const std::string& nodeId) {
        SnapshotGuard guard(*this);
        const RingSnapshot* state = guard.get();
        auto it = state->nodes.find(nodeId);
        if (it != state->nodes.end()) {
            it->second->status.recordFailure();
        }
    }

    void markHealthy(const std::string& nodeId) {
        SnapshotGuard guard(*this);
        const RingSnapshot* state = guard.get();
        auto it = state->nodes.find(nodeId);
        if (it != state->nodes.end()) {
            it->second->status.markHealthy();
        }
    }

    std::vector<NodeConfig> getAllNodes() const {
        SnapshotGuard guard(*this);
        const RingSnapshot* state = guard.get();
        std::vector<NodeConfig> result;
        result.reserve(state->nodes.size());
        for (const auto& [id, node] : state->nodes) {
            result.push_back(node->config);
        }
        return result;
    }

    size_t nodeCount() const {
        SnapshotGuard guard(*this);
        const RingSnapshot* state = guard.get();
        return state->nodes.size();
    }

    size_t virtualNodeCount() const {
        SnapshotGuard guard(*this);
        const RingSnapshot* state = guard.get();
        return state->ring.size();
    }

    bool empty() const {
        SnapshotGuard guard(*this);
        const RingSnapshot* state = guard.get();
        return state->nodes.empty();
    }

    void clear() {
        const RingSnapshot* retired = nullptr;
        {
            SnapshotGuard guard(*this);
            const RingSnapshot* current = guard.get();
            while (true) {
                auto empty = std::make_unique<RingSnapshot>();
                RingSnapshot* next = empty.release();
                const RingSnapshot* expected = current;
                if (m_state.compare_exchange_weak(
                        expected,
                        next,
                        std::memory_order_release,
                        std::memory_order_acquire)) {
                    retired = current;
                    break;
                }
                delete next;
                current = expected;
            }
        }
        retireSnapshot(retired);
    }

private:
    RingSnapshot* cloneSnapshot(const RingSnapshot& source) {
        auto next = std::make_unique<RingSnapshot>();
        next->ring = source.ring;
        next->nodes = source.nodes;
        return next.release();
    }

    void retireSnapshot(const RingSnapshot* snapshot) {
        RingSnapshot* retired = const_cast<RingSnapshot*>(snapshot);
        RingSnapshot* head = m_retired.load(std::memory_order_acquire);
        do {
            retired->next_retired = head;
        } while (!m_retired.compare_exchange_weak(
            head,
            retired,
            std::memory_order_release,
            std::memory_order_acquire));
        collectRetiredSnapshots();
    }

    void collectRetiredSnapshots() const {
        if (m_readers.load(std::memory_order_acquire) != 0) {
            return;
        }

        RingSnapshot* snapshot = m_retired.exchange(nullptr, std::memory_order_acq_rel);
        while (snapshot != nullptr) {
            RingSnapshot* next = snapshot->next_retired;
            delete snapshot;
            snapshot = next;
        }
    }

    size_t m_virtualNodes;
    HashFunc m_hashFunc;
    std::atomic<const RingSnapshot*> m_state;
    mutable std::atomic<RingSnapshot*> m_retired;
    mutable std::atomic<size_t> m_readers;
};

} // namespace galay::utils

#endif // GALAY_UTILS_CONSISTENT_HASH_HPP
