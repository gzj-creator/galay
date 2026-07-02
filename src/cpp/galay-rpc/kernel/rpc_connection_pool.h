/**
 * @file rpc_connection_pool.h
 * @brief RPC连接池
 * @author galay-rpc
 * @version 1.0.0
 *
 * @details 提供按endpoint划分的轻量连接租约池。当前池负责协程安全的容量、
 *          waiter、broken替换和shutdown语义；真实socket生命周期由租约使用方
 *          在租约窗口内创建和关闭。所有Task路径通过AsyncWaiter挂起，不使用
 *          阻塞锁或条件变量。
 */

#ifndef GALAY_RPC_CONNECTION_POOL_H
#define GALAY_RPC_CONNECTION_POOL_H

#include "rpc_channel.h"
#include "../protoc/rpc_error.h"
#include "../../galay-kernel/concurrency/async_waiter.h"
#include "../../galay-kernel/core/task.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace galay::rpc
{

/**
 * @brief RPC服务端点
 *
 * @details 作为连接池key使用，host和port共同唯一标识一个远端。
 */
struct RpcEndpoint {
    std::string host;  ///< endpoint主机地址
    uint16_t port = 0; ///< endpoint端口

    /// @brief 返回 host:port 格式的稳定key
    std::string key() const { return host + ":" + std::to_string(port); }

    friend bool operator==(const RpcEndpoint& lhs, const RpcEndpoint& rhs) {
        return lhs.host == rhs.host && lhs.port == rhs.port;
    }
};

/**
 * @brief 连接池配置
 */
struct RpcConnectionPoolConfig {
    size_t min_connections_per_endpoint = 0;  ///< ensureEndpoint时预热的最小逻辑连接数
    size_t max_connections_per_endpoint = 1;  ///< 单endpoint最大逻辑连接数，0会规范化为1
    size_t max_waiters_per_endpoint = 64;     ///< 单endpoint最大等待协程数
    std::chrono::milliseconds idle_ttl{0};    ///< 预留：空闲连接最大保留时间，0表示不主动淘汰
    std::chrono::milliseconds max_lifetime{0};///< 预留：连接最大生命周期，0表示不按寿命淘汰
};

/**
 * @brief 连接池租约
 *
 * @details 租约是可移动对象，携带endpoint、连接id和broken标记。调用方完成使用后
 *          必须显式release；shutdown后释放会关闭该逻辑连接而不是回收到池中。
 */
class RpcPooledConnection {
public:
    RpcPooledConnection() = default;

    RpcPooledConnection(RpcEndpoint endpoint, uint64_t id)
        : m_endpoint(std::move(endpoint))
        , m_id(id)
        , m_valid(true)
    {
    }

    RpcPooledConnection(const RpcPooledConnection&) = delete;
    RpcPooledConnection& operator=(const RpcPooledConnection&) = delete;

    RpcPooledConnection(RpcPooledConnection&& other) noexcept
        : m_endpoint(std::move(other.m_endpoint))
        , m_id(other.m_id)
        , m_broken(other.m_broken)
        , m_valid(other.m_valid)
    {
        other.m_valid = false;
        other.m_broken = true;
    }

    RpcPooledConnection& operator=(RpcPooledConnection&& other) noexcept {
        if (this == &other) {
            return *this;
        }
        m_endpoint = std::move(other.m_endpoint);
        m_id = other.m_id;
        m_broken = other.m_broken;
        m_valid = other.m_valid;
        other.m_valid = false;
        other.m_broken = true;
        return *this;
    }

    /// @brief 连接id，用于测试和诊断复用/替换行为
    uint64_t id() const { return m_id; }
    /// @brief 租约所属endpoint
    const RpcEndpoint& endpoint() const { return m_endpoint; }
    /// @brief 租约是否仍可归还
    bool valid() const { return m_valid; }
    /// @brief 标记该连接不可复用
    void markBroken() { m_broken = true; }
    /// @brief 查询是否已标记broken
    bool broken() const { return m_broken; }

private:
    friend class RpcConnectionPool;

    RpcEndpoint m_endpoint;
    uint64_t m_id = 0;
    bool m_broken = false;
    bool m_valid = false;
};

using RpcPoolAcquireResult = std::expected<RpcPooledConnection, RpcError>;

/**
 * @brief RPC连接池
 *
 * @details 该类型假定在同一runtime调度上下文内访问内部状态；跨线程唤醒由
 *          AsyncWaiter负责。acquire在容量不足但waiter未满时挂起，release/shutdown
 *          负责唤醒等待者。Task路径不使用阻塞锁。
 */
class RpcConnectionPool {
public:
    explicit RpcConnectionPool(RpcConnectionPoolConfig config = {})
        : m_config(normalize(config))
    {
    }

    RpcConnectionPool(const RpcConnectionPool&) = delete;
    RpcConnectionPool& operator=(const RpcConnectionPool&) = delete;
    RpcConnectionPool(RpcConnectionPool&&) = delete;
    RpcConnectionPool& operator=(RpcConnectionPool&&) = delete;

    /**
     * @brief 初始化endpoint并按min连接数预热逻辑连接
     * @param endpoint 目标endpoint
     * @return 成功或参数/shutdown错误
     */
    std::expected<void, RpcError> ensureEndpoint(const RpcEndpoint& endpoint) {
        if (!isValidEndpoint(endpoint)) {
            return std::unexpected(RpcError(RpcErrorCode::INVALID_REQUEST,
                                            "RPC endpoint is invalid"));
        }
        if (m_shutdown) {
            return std::unexpected(RpcError(RpcErrorCode::CONNECTION_CLOSED,
                                            "RPC connection pool is shut down"));
        }

        auto& bucket = bucketFor(endpoint);
        while (bucket.total < m_config.min_connections_per_endpoint &&
               bucket.total < m_config.max_connections_per_endpoint) {
            bucket.available.push_back(newConnection(endpoint));
            ++bucket.total;
        }
        return {};
    }

    /**
     * @brief 获取endpoint连接租约
     * @param endpoint 目标endpoint
     * @return 连接租约；压力超限返回RESOURCE_EXHAUSTED，shutdown返回CONNECTION_CLOSED
     *
     * @note 容量不足但waiter未满时挂起当前协程，等待release或shutdown唤醒。
     */
    Task<RpcPoolAcquireResult> acquire(const RpcEndpoint& endpoint) {
        auto ready = tryAcquire(endpoint);
        if (ready.has_value() || ready.error().code() != RpcErrorCode::RESOURCE_EXHAUSTED) {
            co_return std::move(ready);
        }

        auto& bucket = bucketFor(endpoint);
        if (bucket.waiters.size() >= m_config.max_waiters_per_endpoint) {
            co_return RpcPoolAcquireResult(std::unexpected(
                RpcError(RpcErrorCode::RESOURCE_EXHAUSTED, "RPC connection pool waiters exhausted")));
        }

        auto waiter = std::make_shared<PoolWaiter>();
        bucket.waiters.push_back(waiter);
        auto wait_result = co_await waiter->waiter.wait();
        if (!wait_result.has_value()) {
            co_return RpcPoolAcquireResult(std::unexpected(
                RpcError(RpcErrorCode::INTERNAL_ERROR, wait_result.error().message())));
        }
        co_return std::move(wait_result.value());
    }

    /**
     * @brief 归还连接租约
     * @param connection acquire返回的租约
     * @return 成功或无效租约错误
     *
     * @details broken连接不会复用；如果仍有waiter且未shutdown，会创建替代连接唤醒
     *          一个waiter。正常连接优先交给等待者，否则回收到available队列。
     */
    std::expected<void, RpcError> release(RpcPooledConnection connection) {
        if (!connection.valid()) {
            return std::unexpected(RpcError(RpcErrorCode::INVALID_REQUEST,
                                            "RPC pooled connection is invalid"));
        }
        auto* bucket = findBucketMutable(connection.endpoint());
        if (bucket == nullptr) {
            return std::unexpected(RpcError(RpcErrorCode::INVALID_REQUEST,
                                            "RPC pooled endpoint is unknown"));
        }

        if (bucket->in_use > 0) {
            --bucket->in_use;
        }

        if (m_shutdown) {
            if (bucket->total > 0) {
                --bucket->total;
            }
            return {};
        }

        if (connection.broken()) {
            if (bucket->total > 0) {
                --bucket->total;
            }
            wakeNextWaiter(*bucket);
            return {};
        }

        if (!bucket->waiters.empty() && wakeNextWaiter(*bucket, std::move(connection))) {
            return {};
        }

        bucket->available.push_back(std::move(connection));
        return {};
    }

    /**
     * @brief 关闭连接池并唤醒所有waiter
     * @return 成功
     */
    std::expected<void, RpcError> shutdown() {
        if (m_shutdown) {
            return {};
        }
        m_shutdown = true;
        RpcError error(RpcErrorCode::CONNECTION_CLOSED, "RPC connection pool shut down");
        for (auto& [_, bucket] : m_buckets) {
            for (auto& waiter : bucket.waiters) {
                waiter->waiter.notify(RpcPoolAcquireResult(std::unexpected(error)));
            }
            bucket.waiters.clear();
            bucket.available.clear();
            bucket.in_use = 0;
            bucket.total = 0;
        }
        return {};
    }

    /// @brief 指定endpoint可复用连接数量
    size_t availableCount(const RpcEndpoint& endpoint) const {
        const auto* bucket = findBucket(endpoint);
        return bucket == nullptr ? 0 : bucket->available.size();
    }

    /// @brief 指定endpoint当前租出数量
    size_t inUseCount(const RpcEndpoint& endpoint) const {
        const auto* bucket = findBucket(endpoint);
        return bucket == nullptr ? 0 : bucket->in_use;
    }

    /// @brief 指定endpoint等待者数量
    size_t waiterCount(const RpcEndpoint& endpoint) const {
        const auto* bucket = findBucket(endpoint);
        return bucket == nullptr ? 0 : bucket->waiters.size();
    }

    /// @brief 指定endpoint总跟踪逻辑连接数
    size_t totalTrackedConnections(const RpcEndpoint& endpoint) const {
        const auto* bucket = findBucket(endpoint);
        return bucket == nullptr ? 0 : bucket->total;
    }

private:
    struct PoolWaiter {
        AsyncWaiter<RpcPoolAcquireResult> waiter;
    };

    struct EndpointBucket {
        RpcEndpoint endpoint;
        std::deque<RpcPooledConnection> available;
        std::deque<std::shared_ptr<PoolWaiter>> waiters;
        size_t in_use = 0;
        size_t total = 0;
    };

    struct EndpointHash {
        size_t operator()(const RpcEndpoint& endpoint) const noexcept {
            const size_t host_hash = std::hash<std::string>{}(endpoint.host);
            const size_t port_hash = std::hash<uint16_t>{}(endpoint.port);
            const size_t mixed_port =
                port_hash + 0x9e3779b97f4a7c15ULL + (host_hash << 6) + (host_hash >> 2);
            return host_hash ^ mixed_port;
        }
    };

    static RpcConnectionPoolConfig normalize(RpcConnectionPoolConfig config) {
        if (config.max_connections_per_endpoint == 0) {
            config.max_connections_per_endpoint = 1;
        }
        if (config.min_connections_per_endpoint > config.max_connections_per_endpoint) {
            config.min_connections_per_endpoint = config.max_connections_per_endpoint;
        }
        return config;
    }

    static bool isValidEndpoint(const RpcEndpoint& endpoint) {
        return !endpoint.host.empty() && endpoint.port != 0;
    }

    RpcPooledConnection newConnection(const RpcEndpoint& endpoint) {
        return RpcPooledConnection(endpoint, m_next_id++);
    }

    EndpointBucket& bucketFor(const RpcEndpoint& endpoint) {
        if (m_last_bucket != nullptr && m_last_bucket->endpoint == endpoint) {
            return *m_last_bucket;
        }

        auto [it, inserted] = m_buckets.try_emplace(endpoint);
        if (inserted) {
            it->second.endpoint = endpoint;
        }
        m_last_bucket = &it->second;
        return it->second;
    }

    const EndpointBucket* findBucket(const RpcEndpoint& endpoint) const {
        if (m_last_bucket != nullptr && m_last_bucket->endpoint == endpoint) {
            return m_last_bucket;
        }

        auto it = m_buckets.find(endpoint);
        if (it == m_buckets.end()) {
            return nullptr;
        }
        return &it->second;
    }

    EndpointBucket* findBucketMutable(const RpcEndpoint& endpoint) {
        if (m_last_bucket != nullptr && m_last_bucket->endpoint == endpoint) {
            return m_last_bucket;
        }

        auto it = m_buckets.find(endpoint);
        if (it == m_buckets.end()) {
            return nullptr;
        }
        m_last_bucket = &it->second;
        return &it->second;
    }

    RpcPoolAcquireResult tryAcquire(const RpcEndpoint& endpoint) {
        if (!isValidEndpoint(endpoint)) {
            return std::unexpected(RpcError(RpcErrorCode::INVALID_REQUEST,
                                            "RPC endpoint is invalid"));
        }
        if (m_shutdown) {
            return std::unexpected(RpcError(RpcErrorCode::CONNECTION_CLOSED,
                                            "RPC connection pool is shut down"));
        }

        auto& bucket = bucketFor(endpoint);
        if (!bucket.available.empty()) {
            auto connection = std::move(bucket.available.front());
            bucket.available.pop_front();
            ++bucket.in_use;
            return connection;
        }
        if (bucket.total < m_config.max_connections_per_endpoint) {
            ++bucket.total;
            ++bucket.in_use;
            return newConnection(endpoint);
        }
        return std::unexpected(RpcError(RpcErrorCode::RESOURCE_EXHAUSTED,
                                        "RPC connection pool is exhausted"));
    }

    bool wakeNextWaiter(EndpointBucket& bucket, RpcPooledConnection connection) {
        while (!bucket.waiters.empty()) {
            auto waiter = std::move(bucket.waiters.front());
            bucket.waiters.pop_front();
            ++bucket.in_use;
            waiter->waiter.notify(RpcPoolAcquireResult(std::move(connection)));
            return true;
        }
        return false;
    }

    bool wakeNextWaiter(EndpointBucket& bucket) {
        while (!bucket.waiters.empty()) {
            if (m_shutdown) {
                return false;
            }
            if (bucket.total >= m_config.max_connections_per_endpoint) {
                return false;
            }
            ++bucket.total;
            auto replacement = newConnection(bucket.endpoint);
            auto waiter = std::move(bucket.waiters.front());
            bucket.waiters.pop_front();
            ++bucket.in_use;
            waiter->waiter.notify(RpcPoolAcquireResult(std::move(replacement)));
            return true;
        }
        return false;
    }

    RpcConnectionPoolConfig m_config;
    std::unordered_map<RpcEndpoint, EndpointBucket, EndpointHash> m_buckets;
    EndpointBucket* m_last_bucket = nullptr;
    uint64_t m_next_id = 1;
    bool m_shutdown = false;
};

} // namespace galay::rpc

#endif // GALAY_RPC_CONNECTION_POOL_H
