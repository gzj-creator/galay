#include "conn_pool.h"
#include <galay/cpp/galay-redis/base/redis_log.h>
#include <algorithm>
#include <cstdint>
#include <functional>
#include <limits>
#include <thread>
#include <type_traits>
#include <utility>

namespace galay::redis
{
    namespace
    {
        template <typename T>
        void incrementCounter(std::atomic<T>& counter,
                              T delta = T{1},
                              std::memory_order order = std::memory_order_acq_rel) noexcept
        requires std::is_integral_v<T>
        {
            const T previous = counter.fetch_add(delta, order);
            if (previous > std::numeric_limits<T>::max() - delta) {
                counter.store(std::numeric_limits<T>::max(), std::memory_order_release);
            }
        }

        void decrementCounter(std::atomic<size_t>& counter) noexcept
        {
            size_t current = counter.load(std::memory_order_acquire);
            while (current > 0) {
                if (counter.compare_exchange_weak(current,
                                                  current - 1,
                                                  std::memory_order_acq_rel,
                                                  std::memory_order_acquire)) {
                    return;
                }
            }
        }

        void decrementActive(std::atomic<size_t>& active) noexcept
        {
            decrementCounter(active);
        }

        template <typename OptionalT, typename... Args>
        bool emplaceOptional(OptionalT& target, Args&&... args)
        {
            auto& stored = target.emplace(std::forward<Args>(args)...);
            return std::addressof(stored) == std::addressof(*target);
        }

        void setAcquireError(std::optional<RedisError>& target,
                             RedisErrorType type,
                             std::string message)
        {
            const bool stored = emplaceOptional(target, type, std::move(message));
            if (!stored) {
                REDIS_LOG_ERROR("[client]", "Failed to store Redis pool acquire error state");
            }
        }

        size_t mixedThreadShardIndex(const void* pool, size_t shard_count) noexcept
        {
            auto h = static_cast<uint64_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
            h ^= static_cast<uint64_t>(reinterpret_cast<uintptr_t>(pool) >> 4);
            h ^= h >> 33;
            h *= 0xff51afd7ed558ccdULL;
            h ^= h >> 33;
            h *= 0xc4ceb9fe1a85ec53ULL;
            h ^= h >> 33;
            return static_cast<size_t>(h & (shard_count - 1));
        }
    } // namespace

    #include "../details/pool_awaitable.inl"

    RedisConnectionPool::RedisConnectionPool(IOScheduler* scheduler, ConnectionPoolConfig config)
        : m_scheduler(scheduler)
        , m_config(std::move(config))
    {
        // 验证配置
        if (!m_config.validate()) {
            REDIS_LOG_ERROR("[client]", "Invalid connection pool configuration");
            m_is_shutting_down.store(true, std::memory_order_release);
        }

        REDIS_LOG_INFO("[client]", "Connection pool created: host={}:{}, min={}, max={}, initial={}",
                     m_config.host, m_config.port,
                     m_config.min_connections, m_config.max_connections,
                     m_config.initial_connections);
    }

    RedisConnectionPool::~RedisConnectionPool()
    {
        if (m_is_initialized && !m_is_shutting_down) {
            REDIS_LOG_WARN("[client]", "Connection pool destroyed without proper shutdown");
            shutdown();
        }
    }

    PoolInitializeAwaitable RedisConnectionPool::initialize()
    {
        return PoolInitializeAwaitable(*this);
    }

    PoolAcquireAwaitable RedisConnectionPool::acquire()
    {
        return PoolAcquireAwaitable(*this);
    }

    RedisVoidResult RedisConnectionPool::initializeSync()
    {
        if (m_is_shutting_down) {
            return std::unexpected(RedisError(
                RedisErrorType::REDIS_ERROR_TYPE_INTERNAL_ERROR,
                "Connection pool is shutting down"
            ));
        }

        if (m_is_initialized) {
            return {};
        }

        m_is_initialized.store(true, std::memory_order_release);
        REDIS_LOG_INFO("[client]", "Connection pool initialized lazily");
        return {};
    }

    std::expected<std::shared_ptr<PooledConnection>, RedisError>
    RedisConnectionPool::acquireSync(std::chrono::steady_clock::time_point start_time)
    {
        if (!m_is_initialized) {
            return std::unexpected(RedisError(
                RedisErrorType::REDIS_ERROR_TYPE_INTERNAL_ERROR,
                "Connection pool not initialized"
            ));
        }

        if (m_is_shutting_down) {
            return std::unexpected(RedisError(
                RedisErrorType::REDIS_ERROR_TYPE_INTERNAL_ERROR,
                "Connection pool is shutting down"
            ));
        }

        struct WaitingGuard {
            std::atomic<size_t>* counter = nullptr;

            ~WaitingGuard()
            {
                if (counter != nullptr) {
                    decrementCounter(*counter);
                }
            }
        };

        incrementCounter(m_waiting_requests);
        WaitingGuard waiting_guard{&m_waiting_requests};

        if (auto conn = tryAcquireAvailable()) {
            incrementCounter(m_total_acquired);
            incrementCounter(m_active_connections);
            recordAcquireStats(start_time);
            return conn;
        }

        auto result = getConnectionSync();
        if (result) {
            auto conn = result.value();
            conn->updateLastUsed();
            incrementCounter(m_total_acquired);
            incrementCounter(m_active_connections);
            recordAcquireStats(start_time);

            REDIS_LOG_DEBUG("[client]", "Created and acquired new connection, total: {}",
                         m_live_connections.load(std::memory_order_acquire));
            return conn;
        }

        REDIS_LOG_WARN("[client]", "Failed to create new connection: {}", result.error().message());
        return std::unexpected(result.error());
    }

    void RedisConnectionPool::recordAcquireStats(std::chrono::steady_clock::time_point start_time)
    {
        const auto elapsed = std::chrono::steady_clock::now() - start_time;
        const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
        const auto elapsed_ms_u64 = static_cast<uint64_t>(elapsed_ms < 0 ? 0 : elapsed_ms);
        incrementCounter(m_total_acquire_time_ms, elapsed_ms_u64);

        double current_max = m_max_acquire_time_ms.load();
        while (elapsed_ms > current_max) {
            if (m_max_acquire_time_ms.compare_exchange_weak(current_max, elapsed_ms)) {
                break;
            }
        }

        const size_t active = m_active_connections.load(std::memory_order_acquire);
        size_t current_peak = m_peak_active_connections.load();
        while (active > current_peak) {
            if (m_peak_active_connections.compare_exchange_weak(current_peak, active)) {
                break;
            }
        }
    }

    size_t RedisConnectionPool::idleShardIndex() const noexcept
    {
        return mixedThreadShardIndex(this, kIdleShardCount);
    }

    std::shared_ptr<PooledConnection> RedisConnectionPool::tryAcquireAvailable()
    {
        const size_t start = idleShardIndex();
        for (size_t i = 0; i < kIdleShardCount; ++i) {
            auto& shard = m_available_shards[(start + i) & (kIdleShardCount - 1)];
            std::shared_ptr<PooledConnection> conn;
            while (shard.available.try_dequeue(conn)) {
                decrementCounter(m_idle_connections);
                if (conn && !conn->isClosed() && conn->isHealthy()) {
                    conn->updateLastUsed();
                    return conn;
                }
                destroyConnectionSlot(conn);
            }
        }
        return nullptr;
    }

    std::shared_ptr<PooledConnection> RedisConnectionPool::createConnectionSlot()
    {
        size_t current = m_live_connections.load(std::memory_order_acquire);
        while (current < m_config.max_connections) {
            if (m_live_connections.compare_exchange_weak(current,
                                                         current + 1,
                                                         std::memory_order_acq_rel,
                                                         std::memory_order_acquire)) {
                auto client = std::make_shared<RedisClient<>>(m_scheduler);
                auto conn = std::make_shared<PooledConnection>(client, m_scheduler);
                incrementCounter(m_total_created);
                return conn;
            }
        }
        return nullptr;
    }

    void RedisConnectionPool::destroyConnectionSlot(std::shared_ptr<PooledConnection>& conn)
    {
        if (!conn) {
            return;
        }
        conn->setHealthy(false);
        conn.reset();
        decrementCounter(m_live_connections);
        incrementCounter(m_total_destroyed);
    }

    bool RedisConnectionPool::returnToAvailable(std::shared_ptr<PooledConnection> conn)
    {
        if (!conn) {
            return false;
        }
        const size_t shard_index = idleShardIndex();
        auto queued = conn;
        if (!m_available_shards[shard_index].available.enqueue(std::move(queued))) {
            destroyConnectionSlot(conn);
            return false;
        }
        incrementCounter(m_idle_connections);
        return true;
    }

    bool RedisConnectionPool::enqueueWaiter(std::shared_ptr<detail::RedisPoolWaiter> waiter)
    {
        return waiter != nullptr && m_waiters.enqueue(std::move(waiter));
    }

    bool RedisConnectionPool::completeOneWaiter(
        std::shared_ptr<PooledConnection> conn,
        std::shared_ptr<detail::RedisPoolWaiter>& waiter_to_wake)
    {
        if (!conn) {
            return false;
        }

        std::shared_ptr<detail::RedisPoolWaiter> waiter;
        while (m_waiters.try_dequeue(waiter)) {
            if (waiter == nullptr) {
                continue;
            }

            conn->updateLastUsed();
            waiter->connection = conn;
            if (!detail::try_complete_waiter(waiter->state,
                                             detail::PoolWaiterState::Completed)) {
                waiter->connection.reset();
                continue;
            }

            waiter_to_wake = std::move(waiter);
            return true;
        }
        return false;
    }

    bool RedisConnectionPool::wakeOneWaiterFromAvailable()
    {
        auto conn = tryAcquireAvailable();
        if (!conn) {
            return false;
        }

        std::shared_ptr<detail::RedisPoolWaiter> waiter_to_wake;
        if (!completeOneWaiter(conn, waiter_to_wake)) {
            const bool returned = returnToAvailable(std::move(conn));
            if (!returned) {
                REDIS_LOG_WARN("[client]", "Failed to return Redis connection after waiter wake race");
            }
            return false;
        }

        waiter_to_wake->waker.wakeUp();
        return true;
    }

    size_t RedisConnectionPool::drainAvailableConnections(
        std::vector<std::shared_ptr<PooledConnection>>* drained)
    {
        size_t count = 0;
        for (auto& shard : m_available_shards) {
            std::shared_ptr<PooledConnection> conn;
            while (shard.available.try_dequeue(conn)) {
                decrementCounter(m_idle_connections);
                ++count;
                if (drained != nullptr && conn) {
                    drained->push_back(std::move(conn));
                }
            }
        }
        return count;
    }

    void RedisConnectionPool::release(std::shared_ptr<PooledConnection> conn)
    {
        if (!conn) {
            return;
        }

        if (m_is_shutting_down) {
            REDIS_LOG_DEBUG("[client]", "Connection released during shutdown, will be destroyed");
            decrementActive(m_active_connections);
            return;
        }

        std::shared_ptr<detail::RedisPoolWaiter> waiter_to_wake;

        // 检查连接是否健康
        if (conn->isClosed() || !conn->isHealthy()) {
            REDIS_LOG_WARN("[client]", "Unhealthy connection released, removing from pool");
            destroyConnectionSlot(conn);
            decrementActive(m_active_connections);
            return;
        }

        // 如果连接数超过最大值，销毁连接
        if (m_live_connections.load(std::memory_order_acquire) > m_config.max_connections) {
            REDIS_LOG_DEBUG("[client]", "Pool size exceeds max, destroying connection");
            destroyConnectionSlot(conn);
            decrementActive(m_active_connections);
            return;
        }

        const bool completed_waiter = completeOneWaiter(conn, waiter_to_wake);
        if (!completed_waiter) {
            // 归还到可用连接池
            const bool returned = returnToAvailable(conn);
            if (!returned) {
                REDIS_LOG_WARN("[client]", "Failed to return Redis connection to idle pool");
            }
        }
        incrementCounter(m_total_released);
        decrementActive(m_active_connections);

        REDIS_LOG_DEBUG("[client]", "Connection released to pool, available: {}, total: {}",
                     m_idle_connections.load(std::memory_order_acquire),
                     m_live_connections.load(std::memory_order_acquire));

        if (waiter_to_wake) {
            waiter_to_wake->waker.wakeUp();
        }
    }

    std::expected<std::shared_ptr<PooledConnection>, RedisError>
    RedisConnectionPool::getConnectionSync()
    {
        REDIS_LOG_DEBUG("[client]", "Creating new connection to {}:{}", m_config.host, m_config.port);

        // 带重试的连接创建
        for (int attempt = 0; attempt < m_config.max_reconnect_attempts; ++attempt) {
            if (attempt > 0) {
                incrementCounter(m_reconnect_attempts);
                REDIS_LOG_INFO("[client]", "Reconnect attempt {}/{} for {}:{}",
                            attempt + 1, m_config.max_reconnect_attempts,
                            m_config.host, m_config.port);
            }

            auto conn = createConnectionSlot();
            if (!conn) {
                return std::unexpected(RedisError(
                    RedisErrorType::REDIS_ERROR_TYPE_TIMEOUT_ERROR,
                    "Connection pool exhausted"));
            }

            if (attempt > 0) {
                incrementCounter(m_reconnect_successes);
                REDIS_LOG_INFO("[client]", "Reconnect succeeded on attempt {}", attempt + 1);
            }

            REDIS_LOG_DEBUG("[client]", "Connection created successfully, total: {}",
                         m_live_connections.load(std::memory_order_acquire));
            return conn;
        }

        return std::unexpected(RedisError(
            RedisErrorType::REDIS_ERROR_TYPE_CONNECTION_ERROR,
            "Failed to create connection"
        ));
    }

    bool RedisConnectionPool::checkConnectionHealthSync(std::shared_ptr<PooledConnection> conn)
    {
        if (!conn || conn->isClosed()) {
            return false;
        }

        // TODO: 实现同步健康检查
        // 在实际使用中，需要在协程上下文中调用 co_await conn->get()->ping()

        return conn->isHealthy();
    }

    void RedisConnectionPool::triggerHealthCheck()
    {
        if (!m_config.enable_health_check) {
            return;
        }

        REDIS_LOG_INFO("[client]", "Running health check on {} connections",
                     m_live_connections.load(std::memory_order_acquire));
        const size_t removed = cleanupUnhealthyConnections();
        if (removed > 0) {
            REDIS_LOG_WARN("[client]", "Removed {} unhealthy connections, remaining: {}",
                        removed, m_live_connections.load(std::memory_order_acquire));
        }

        // 如果连接数低于最小值，创建新连接
        size_t current_size = m_live_connections.load(std::memory_order_acquire);
        while (current_size < m_config.min_connections) {
            auto result = getConnectionSync();
            if (!result) {
                REDIS_LOG_ERROR("[client]", "Failed to create replacement connection: {}",
                             result.error().message());
                break;
            }

            auto conn = result.value();
            if (!returnToAvailable(conn)) {
                REDIS_LOG_ERROR("[client]", "Failed to enqueue replacement connection");
                break;
            }
            current_size = m_live_connections.load(std::memory_order_acquire);

            REDIS_LOG_INFO("[client]", "Created replacement connection, total: {}", current_size);
        }
    }

    void RedisConnectionPool::triggerIdleCleanup()
    {
        REDIS_LOG_INFO("[client]", "Running idle connection cleanup");

        std::vector<std::shared_ptr<PooledConnection>> idle_connections;
        const size_t drained_count = drainAvailableConnections(&idle_connections);
        if (drained_count == 0) {
            return;
        }

        size_t removed = 0;
        for (auto& conn : idle_connections) {
            if (conn && conn->getIdleTime() > m_config.idle_timeout &&
                m_live_connections.load(std::memory_order_acquire) > m_config.min_connections) {
                destroyConnectionSlot(conn);
                ++removed;
            } else if (conn) {
                const bool returned = returnToAvailable(conn);
                if (!returned) {
                    REDIS_LOG_WARN("[client]", "Failed to return Redis connection during idle cleanup");
                }
            }
        }

        if (removed > 0) {
            REDIS_LOG_INFO("[client]", "Cleaned up {} idle connections, remaining: {}",
                        removed, m_live_connections.load(std::memory_order_acquire));
        }
    }

    void RedisConnectionPool::warmup()
    {
        REDIS_LOG_INFO("[client]", "Warming up connection pool to {} connections", m_config.min_connections);

        size_t current_size = m_live_connections.load(std::memory_order_acquire);
        size_t created = 0;
        while (current_size < m_config.min_connections) {
            auto result = getConnectionSync();
            if (!result) {
                REDIS_LOG_ERROR("[client]", "Failed to create warmup connection: {}", result.error().message());
                break;
            }

            auto conn = result.value();
            if (!returnToAvailable(conn)) {
                REDIS_LOG_ERROR("[client]", "Failed to enqueue warmup connection");
                break;
            }
            current_size = m_live_connections.load(std::memory_order_acquire);
            created++;
        }

        REDIS_LOG_INFO("[client]", "Warmup complete, created {} connections, total: {}", created, current_size);
    }

    size_t RedisConnectionPool::cleanupUnhealthyConnections()
    {
        REDIS_LOG_INFO("[client]", "Cleaning up unhealthy connections");

        std::vector<std::shared_ptr<PooledConnection>> idle_connections;
        const size_t drained_count = drainAvailableConnections(&idle_connections);
        if (drained_count == 0) {
            return 0;
        }

        size_t removed = 0;
        for (auto& conn : idle_connections) {
            if (!conn || conn->isClosed() || !conn->isHealthy()) {
                destroyConnectionSlot(conn);
                ++removed;
            } else {
                const bool returned = returnToAvailable(conn);
                if (!returned) {
                    REDIS_LOG_WARN("[client]", "Failed to return Redis connection during health cleanup");
                }
            }
        }

        if (removed > 0) {
            REDIS_LOG_INFO("[client]", "Cleaned up {} unhealthy connections, remaining: {}",
                        removed, m_live_connections.load(std::memory_order_acquire));
        }

        return removed;
    }

    size_t RedisConnectionPool::expandPool(size_t count)
    {
        if (count == 0) {
            return 0;
        }

        REDIS_LOG_INFO("[client]", "Expanding pool by {} connections", count);

        size_t created = 0;
        for (size_t i = 0; i < count; ++i) {
            const size_t current_size = m_live_connections.load(std::memory_order_acquire);

            // 检查是否超过最大连接数
            if (current_size >= m_config.max_connections) {
                REDIS_LOG_WARN("[client]", "Cannot expand pool: reached max connections ({})", m_config.max_connections);
                break;
            }

            auto result = getConnectionSync();
            if (!result) {
                REDIS_LOG_ERROR("[client]", "Failed to create connection during expansion: {}",
                             result.error().message());
                break;
            }

            auto conn = result.value();
            if (!returnToAvailable(conn)) {
                REDIS_LOG_ERROR("[client]", "Failed to enqueue expanded connection");
                break;
            }
            created++;
        }

        REDIS_LOG_INFO("[client]", "Pool expansion complete, created {} connections, total: {}",
                     created, m_live_connections.load(std::memory_order_acquire));
        return created;
    }

    size_t RedisConnectionPool::shrinkPool(size_t target_size)
    {
        REDIS_LOG_INFO("[client]", "Shrinking pool to {} connections", target_size);

        // 确保不低于最小连接数
        if (target_size < m_config.min_connections) {
            target_size = m_config.min_connections;
            REDIS_LOG_WARN("[client]", "Target size adjusted to min_connections: {}", target_size);
        }

        if (m_live_connections.load(std::memory_order_acquire) <= target_size) {
            REDIS_LOG_INFO("[client]", "Current size ({}) <= target size ({}), no shrink needed",
                        m_live_connections.load(std::memory_order_acquire), target_size);
            return 0;
        }

        size_t removed = 0;
        while (m_live_connections.load(std::memory_order_acquire) > target_size) {
            auto conn = tryAcquireAvailable();
            if (!conn) {
                break;
            }
            destroyConnectionSlot(conn);
            ++removed;
        }

        REDIS_LOG_INFO("[client]", "Pool shrink complete, removed {} connections, remaining: {}",
                     removed, m_live_connections.load(std::memory_order_acquire));
        return removed;
    }

    void RedisConnectionPool::shutdown()
    {
        if (m_is_shutting_down.exchange(true, std::memory_order_acq_rel)) {
            return;
        }

        REDIS_LOG_INFO("[client]", "Shutting down connection pool");

        std::vector<std::shared_ptr<PooledConnection>> idle_connections;
        std::vector<std::shared_ptr<detail::RedisPoolWaiter>> waiters_to_wake;

        const size_t drained_count = drainAvailableConnections(&idle_connections);

        std::shared_ptr<detail::RedisPoolWaiter> waiter;
        while (m_waiters.try_dequeue(waiter)) {
            if (waiter == nullptr) {
                continue;
            }
            if (detail::try_complete_waiter(waiter->state,
                                            detail::PoolWaiterState::Cancelled)) {
                waiters_to_wake.push_back(std::move(waiter));
            }
        }
        m_active_connections.store(0, std::memory_order_release);
        m_idle_connections.store(0, std::memory_order_release);
        m_live_connections.store(0, std::memory_order_release);

        for (auto& waiter : waiters_to_wake) {
            waiter->waker.wakeUp();
        }

        m_is_initialized.store(false, std::memory_order_release);
        REDIS_LOG_INFO("[client]", "Connection pool shutdown complete, closed {} connections",
                     drained_count);
    }

    RedisConnectionPool::PoolStats RedisConnectionPool::getStats() const
    {
        PoolStats stats;
        stats.total_connections = m_live_connections.load(std::memory_order_acquire);
        stats.available_connections = m_idle_connections.load(std::memory_order_acquire);
        stats.active_connections = m_active_connections.load(std::memory_order_acquire);
        stats.waiting_requests = m_waiting_requests.load();
        stats.total_acquired = m_total_acquired.load();
        stats.total_released = m_total_released.load();
        stats.total_created = m_total_created.load();
        stats.total_destroyed = m_total_destroyed.load();
        stats.health_check_failures = m_health_check_failures.load();
        stats.reconnect_attempts = m_reconnect_attempts.load();
        stats.reconnect_successes = m_reconnect_successes.load();
        stats.validation_failures = m_validation_failures.load();

        // 性能监控指标
        stats.total_acquire_time_ms = m_total_acquire_time_ms.load();
        stats.max_acquire_time_ms = m_max_acquire_time_ms.load();
        stats.peak_active_connections = m_peak_active_connections.load();

        // 计算平均获取时间
        if (stats.total_acquired > 0) {
            stats.avg_acquire_time_ms = static_cast<double>(stats.total_acquire_time_ms) / stats.total_acquired;
        } else {
            stats.avg_acquire_time_ms = 0.0;
        }

        return stats;
    }

#ifdef GALAY_SSL_FEATURE_ENABLED

    RedissConnectionPool::RedissConnectionPool(IOScheduler* scheduler, RedissConnectionPoolConfig config)
        : m_scheduler(scheduler)
        , m_config(std::move(config))
    {
        if (!m_config.validate()) {
            REDIS_LOG_ERROR("[client]", "Invalid TLS connection pool configuration");
            m_is_shutting_down.store(true, std::memory_order_release);
        }

        REDIS_LOG_INFO("[client]", "TLS connection pool created: host={}:{}, min={}, max={}, initial={}",
                     m_config.host, m_config.port,
                     m_config.min_connections, m_config.max_connections,
                     m_config.initial_connections);
    }

    RedissConnectionPool::~RedissConnectionPool()
    {
        if (m_is_initialized && !m_is_shutting_down) {
            REDIS_LOG_WARN("[client]", "TLS connection pool destroyed without proper shutdown");
            shutdown();
        }
    }

    RedissPoolInitializeAwaitable RedissConnectionPool::initialize()
    {
        return RedissPoolInitializeAwaitable(*this);
    }

    RedissPoolAcquireAwaitable RedissConnectionPool::acquire()
    {
        return RedissPoolAcquireAwaitable(*this);
    }

    RedisVoidResult RedissConnectionPool::initializeSync()
    {
        if (m_is_shutting_down) {
            return std::unexpected(RedisError(
                RedisErrorType::REDIS_ERROR_TYPE_INTERNAL_ERROR,
                "TLS connection pool is shutting down"
            ));
        }

        if (m_is_initialized) {
            return {};
        }

        size_t created_count = 0;
        while (created_count < m_config.initial_connections) {
            auto result = getConnectionSync();
            if (!result) {
                REDIS_LOG_ERROR("[client]", "Failed to create TLS connection {}/{}: {}",
                              created_count + 1, m_config.initial_connections,
                              result.error().message());
                break;
            }

            auto conn = result.value();
            if (!returnToAvailable(conn)) {
                REDIS_LOG_ERROR("[client]", "Failed to enqueue initial TLS connection");
                break;
            }
            ++created_count;
        }

        if (created_count < m_config.min_connections) {
            return std::unexpected(RedisError(
                RedisErrorType::REDIS_ERROR_TYPE_CONNECTION_ERROR,
                "Failed to create minimum TLS connections"
            ));
        }

        m_is_initialized.store(true, std::memory_order_release);
        REDIS_LOG_INFO("[client]", "TLS connection pool initialized with {} connections", created_count);
        return {};
    }

    std::expected<std::shared_ptr<PooledRedissConnection>, RedisError>
    RedissConnectionPool::acquireSync(std::chrono::steady_clock::time_point start_time)
    {
        if (!m_is_initialized) {
            return std::unexpected(RedisError(
                RedisErrorType::REDIS_ERROR_TYPE_INTERNAL_ERROR,
                "TLS connection pool not initialized"
            ));
        }

        if (m_is_shutting_down) {
            return std::unexpected(RedisError(
                RedisErrorType::REDIS_ERROR_TYPE_INTERNAL_ERROR,
                "TLS connection pool is shutting down"
            ));
        }

        struct WaitingGuard {
            std::atomic<size_t>* counter = nullptr;

            ~WaitingGuard()
            {
                if (counter != nullptr) {
                    decrementCounter(*counter);
                }
            }
        };

        incrementCounter(m_waiting_requests);
        WaitingGuard waiting_guard{&m_waiting_requests};

        if (auto conn = tryAcquireAvailable()) {
            incrementCounter(m_total_acquired);
            incrementCounter(m_active_connections);
            recordAcquireStats(start_time);
            return conn;
        }

        auto result = getConnectionSync();
        if (result) {
            auto conn = result.value();
            conn->updateLastUsed();
            incrementCounter(m_total_acquired);
            incrementCounter(m_active_connections);
            recordAcquireStats(start_time);

            REDIS_LOG_DEBUG("[client]", "Created and acquired new TLS connection, total: {}",
                         m_live_connections.load(std::memory_order_acquire));
            return conn;
        }

        REDIS_LOG_WARN("[client]", "Failed to create new TLS connection: {}", result.error().message());
        return std::unexpected(result.error());
    }

    void RedissConnectionPool::recordAcquireStats(std::chrono::steady_clock::time_point start_time)
    {
        const auto elapsed = std::chrono::steady_clock::now() - start_time;
        const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
        const auto elapsed_ms_u64 = static_cast<uint64_t>(elapsed_ms < 0 ? 0 : elapsed_ms);
        incrementCounter(m_total_acquire_time_ms, elapsed_ms_u64);

        double current_max = m_max_acquire_time_ms.load();
        while (elapsed_ms > current_max) {
            if (m_max_acquire_time_ms.compare_exchange_weak(current_max, elapsed_ms)) {
                break;
            }
        }

        const size_t active = m_active_connections.load(std::memory_order_acquire);
        size_t current_peak = m_peak_active_connections.load();
        while (active > current_peak) {
            if (m_peak_active_connections.compare_exchange_weak(current_peak, active)) {
                break;
            }
        }
    }

    size_t RedissConnectionPool::idleShardIndex() const noexcept
    {
        return mixedThreadShardIndex(this, kIdleShardCount);
    }

    std::shared_ptr<PooledRedissConnection> RedissConnectionPool::tryAcquireAvailable()
    {
        const size_t start = idleShardIndex();
        for (size_t i = 0; i < kIdleShardCount; ++i) {
            auto& shard = m_available_shards[(start + i) & (kIdleShardCount - 1)];
            std::shared_ptr<PooledRedissConnection> conn;
            while (shard.available.try_dequeue(conn)) {
                decrementCounter(m_idle_connections);
                if (conn && !conn->isClosed() && conn->isHealthy()) {
                    conn->updateLastUsed();
                    return conn;
                }
                destroyConnectionSlot(conn);
            }
        }
        return nullptr;
    }

    std::shared_ptr<PooledRedissConnection> RedissConnectionPool::createConnectionSlot()
    {
        size_t current = m_live_connections.load(std::memory_order_acquire);
        while (current < m_config.max_connections) {
            if (m_live_connections.compare_exchange_weak(current,
                                                         current + 1,
                                                         std::memory_order_acq_rel,
                                                         std::memory_order_acquire)) {
                AsyncRedisConfig async_config;
                async_config.send_timeout = m_config.connect_timeout;
                async_config.recv_timeout = m_config.connect_timeout;
                auto client = std::make_shared<RedissClient>(
                    m_scheduler,
                    async_config,
                    m_config.tls_config);
                auto conn = std::make_shared<PooledRedissConnection>(client, m_scheduler);
                incrementCounter(m_total_created);
                return conn;
            }
        }
        return nullptr;
    }

    void RedissConnectionPool::destroyConnectionSlot(std::shared_ptr<PooledRedissConnection>& conn)
    {
        if (!conn) {
            return;
        }
        conn->setHealthy(false);
        conn.reset();
        decrementCounter(m_live_connections);
        incrementCounter(m_total_destroyed);
    }

    bool RedissConnectionPool::returnToAvailable(std::shared_ptr<PooledRedissConnection> conn)
    {
        if (!conn) {
            return false;
        }
        const size_t shard_index = idleShardIndex();
        auto queued = conn;
        if (!m_available_shards[shard_index].available.enqueue(std::move(queued))) {
            destroyConnectionSlot(conn);
            return false;
        }
        incrementCounter(m_idle_connections);
        return true;
    }

    bool RedissConnectionPool::enqueueWaiter(std::shared_ptr<detail::RedissPoolWaiter> waiter)
    {
        return waiter != nullptr && m_waiters.enqueue(std::move(waiter));
    }

    bool RedissConnectionPool::completeOneWaiter(
        std::shared_ptr<PooledRedissConnection> conn,
        std::shared_ptr<detail::RedissPoolWaiter>& waiter_to_wake)
    {
        if (!conn) {
            return false;
        }

        std::shared_ptr<detail::RedissPoolWaiter> waiter;
        while (m_waiters.try_dequeue(waiter)) {
            if (waiter == nullptr) {
                continue;
            }

            conn->updateLastUsed();
            waiter->connection = conn;
            if (!detail::try_complete_waiter(waiter->state,
                                             detail::PoolWaiterState::Completed)) {
                waiter->connection.reset();
                continue;
            }

            waiter_to_wake = std::move(waiter);
            return true;
        }
        return false;
    }

    bool RedissConnectionPool::wakeOneWaiterFromAvailable()
    {
        auto conn = tryAcquireAvailable();
        if (!conn) {
            return false;
        }

        std::shared_ptr<detail::RedissPoolWaiter> waiter_to_wake;
        if (!completeOneWaiter(conn, waiter_to_wake)) {
            const bool returned = returnToAvailable(std::move(conn));
            if (!returned) {
                REDIS_LOG_WARN("[client]", "Failed to return TLS Redis connection after waiter wake race");
            }
            return false;
        }

        waiter_to_wake->waker.wakeUp();
        return true;
    }

    size_t RedissConnectionPool::drainAvailableConnections(
        std::vector<std::shared_ptr<PooledRedissConnection>>* drained)
    {
        size_t count = 0;
        for (auto& shard : m_available_shards) {
            std::shared_ptr<PooledRedissConnection> conn;
            while (shard.available.try_dequeue(conn)) {
                decrementCounter(m_idle_connections);
                ++count;
                if (drained != nullptr && conn) {
                    drained->push_back(std::move(conn));
                }
            }
        }
        return count;
    }

    void RedissConnectionPool::release(std::shared_ptr<PooledRedissConnection> conn)
    {
        if (!conn) {
            return;
        }

        if (m_is_shutting_down) {
            REDIS_LOG_DEBUG("[client]", "TLS connection released during shutdown, will be destroyed");
            decrementActive(m_active_connections);
            return;
        }

        std::shared_ptr<detail::RedissPoolWaiter> waiter_to_wake;

        if (conn->isClosed() || !conn->isHealthy()) {
            REDIS_LOG_WARN("[client]", "Unhealthy TLS connection released, removing from pool");
            destroyConnectionSlot(conn);
            decrementActive(m_active_connections);
            return;
        }

        if (m_live_connections.load(std::memory_order_acquire) > m_config.max_connections) {
            REDIS_LOG_DEBUG("[client]", "TLS pool size exceeds max, destroying connection");
            destroyConnectionSlot(conn);
            decrementActive(m_active_connections);
            return;
        }

        const bool completed_waiter = completeOneWaiter(conn, waiter_to_wake);
        if (!completed_waiter) {
            const bool returned = returnToAvailable(conn);
            if (!returned) {
                REDIS_LOG_WARN("[client]", "Failed to return TLS Redis connection to idle pool");
            }
        }
        incrementCounter(m_total_released);
        decrementActive(m_active_connections);

        REDIS_LOG_DEBUG("[client]", "TLS connection released to pool, available: {}, total: {}",
                      m_idle_connections.load(std::memory_order_acquire),
                      m_live_connections.load(std::memory_order_acquire));

        if (waiter_to_wake) {
            waiter_to_wake->waker.wakeUp();
        }
    }

    std::expected<std::shared_ptr<PooledRedissConnection>, RedisError>
    RedissConnectionPool::getConnectionSync()
    {
        REDIS_LOG_DEBUG("[client]", "Creating new TLS connection to {}:{}", m_config.host, m_config.port);

        for (int attempt = 0; attempt < m_config.max_reconnect_attempts; ++attempt) {
            if (attempt > 0) {
                incrementCounter(m_reconnect_attempts);
                REDIS_LOG_INFO("[client]", "TLS reconnect attempt {}/{} for {}:{}",
                             attempt + 1, m_config.max_reconnect_attempts,
                             m_config.host, m_config.port);
            }

            auto conn = createConnectionSlot();
            if (!conn) {
                return std::unexpected(RedisError(
                    RedisErrorType::REDIS_ERROR_TYPE_TIMEOUT_ERROR,
                    "TLS connection pool exhausted"));
            }

            if (attempt > 0) {
                incrementCounter(m_reconnect_successes);
                REDIS_LOG_INFO("[client]", "TLS reconnect succeeded on attempt {}", attempt + 1);
            }

            REDIS_LOG_DEBUG("[client]", "TLS connection created successfully, total: {}",
                          m_live_connections.load(std::memory_order_acquire));
            return conn;
        }

        return std::unexpected(RedisError(
            RedisErrorType::REDIS_ERROR_TYPE_CONNECTION_ERROR,
            "Failed to create TLS connection"
        ));
    }

    bool RedissConnectionPool::checkConnectionHealthSync(std::shared_ptr<PooledRedissConnection> conn)
    {
        if (!conn || conn->isClosed()) {
            return false;
        }

        return conn->isHealthy();
    }

    void RedissConnectionPool::triggerHealthCheck()
    {
        if (!m_config.enable_health_check) {
            return;
        }

        REDIS_LOG_INFO("[client]", "Running TLS health check on {} connections",
                     m_live_connections.load(std::memory_order_acquire));
        const size_t removed = cleanupUnhealthyConnections();
        if (removed > 0) {
            REDIS_LOG_WARN("[client]", "Removed {} unhealthy TLS connections, remaining: {}",
                         removed, m_live_connections.load(std::memory_order_acquire));
        }

        size_t current_size = m_live_connections.load(std::memory_order_acquire);
        while (current_size < m_config.min_connections) {
            auto result = getConnectionSync();
            if (!result) {
                REDIS_LOG_ERROR("[client]", "Failed to create replacement TLS connection: {}",
                              result.error().message());
                break;
            }

            auto conn = result.value();
            if (!returnToAvailable(conn)) {
                REDIS_LOG_ERROR("[client]", "Failed to enqueue replacement TLS connection");
                break;
            }
            current_size = m_live_connections.load(std::memory_order_acquire);

            REDIS_LOG_INFO("[client]", "Created replacement TLS connection, total: {}", current_size);
        }
    }

    void RedissConnectionPool::triggerIdleCleanup()
    {
        REDIS_LOG_INFO("[client]", "Running TLS idle connection cleanup");

        std::vector<std::shared_ptr<PooledRedissConnection>> idle_connections;
        const size_t drained_count = drainAvailableConnections(&idle_connections);
        if (drained_count == 0) {
            return;
        }

        size_t removed = 0;
        for (auto& conn : idle_connections) {
            if (conn && conn->getIdleTime() > m_config.idle_timeout &&
                m_live_connections.load(std::memory_order_acquire) > m_config.min_connections) {
                destroyConnectionSlot(conn);
                ++removed;
            } else if (conn) {
                const bool returned = returnToAvailable(conn);
                if (!returned) {
                    REDIS_LOG_WARN("[client]", "Failed to return TLS Redis connection during idle cleanup");
                }
            }
        }

        if (removed > 0) {
            REDIS_LOG_INFO("[client]", "Cleaned up {} idle TLS connections, remaining: {}",
                         removed, m_live_connections.load(std::memory_order_acquire));
        }
    }

    void RedissConnectionPool::warmup()
    {
        REDIS_LOG_INFO("[client]", "Warming up TLS connection pool to {} connections", m_config.min_connections);

        size_t current_size = m_live_connections.load(std::memory_order_acquire);
        size_t created = 0;
        while (current_size < m_config.min_connections) {
            auto result = getConnectionSync();
            if (!result) {
                REDIS_LOG_ERROR("[client]", "Failed to create warmup TLS connection: {}",
                              result.error().message());
                break;
            }

            auto conn = result.value();
            if (!returnToAvailable(conn)) {
                REDIS_LOG_ERROR("[client]", "Failed to enqueue warmup TLS connection");
                break;
            }
            current_size = m_live_connections.load(std::memory_order_acquire);
            created++;
        }

        REDIS_LOG_INFO("[client]", "TLS warmup complete, created {} connections, total: {}", created, current_size);
    }

    size_t RedissConnectionPool::cleanupUnhealthyConnections()
    {
        REDIS_LOG_INFO("[client]", "Cleaning up unhealthy TLS connections");

        std::vector<std::shared_ptr<PooledRedissConnection>> idle_connections;
        const size_t drained_count = drainAvailableConnections(&idle_connections);
        if (drained_count == 0) {
            return 0;
        }

        size_t removed = 0;
        for (auto& conn : idle_connections) {
            if (!conn || conn->isClosed() || !conn->isHealthy()) {
                destroyConnectionSlot(conn);
                ++removed;
            } else {
                const bool returned = returnToAvailable(conn);
                if (!returned) {
                    REDIS_LOG_WARN("[client]", "Failed to return TLS Redis connection during health cleanup");
                }
            }
        }

        if (removed > 0) {
            REDIS_LOG_INFO("[client]", "Cleaned up {} unhealthy TLS connections, remaining: {}",
                         removed, m_live_connections.load(std::memory_order_acquire));
        }

        return removed;
    }

    size_t RedissConnectionPool::expandPool(size_t count)
    {
        if (count == 0) {
            return 0;
        }

        REDIS_LOG_INFO("[client]", "Expanding TLS pool by {} connections", count);

        size_t created = 0;
        for (size_t i = 0; i < count; ++i) {
            const size_t current_size = m_live_connections.load(std::memory_order_acquire);

            if (current_size >= m_config.max_connections) {
                REDIS_LOG_WARN("[client]", "Cannot expand TLS pool: reached max connections ({})", m_config.max_connections);
                break;
            }

            auto result = getConnectionSync();
            if (!result) {
                REDIS_LOG_ERROR("[client]", "Failed to create TLS connection during expansion: {}",
                              result.error().message());
                break;
            }

            auto conn = result.value();
            if (!returnToAvailable(conn)) {
                REDIS_LOG_ERROR("[client]", "Failed to enqueue expanded TLS connection");
                break;
            }
            created++;
        }

        REDIS_LOG_INFO("[client]", "TLS pool expansion complete, created {} connections, total: {}",
                     created, m_live_connections.load(std::memory_order_acquire));
        return created;
    }

    size_t RedissConnectionPool::shrinkPool(size_t target_size)
    {
        REDIS_LOG_INFO("[client]", "Shrinking TLS pool to {} connections", target_size);

        if (target_size < m_config.min_connections) {
            target_size = m_config.min_connections;
            REDIS_LOG_WARN("[client]", "TLS target size adjusted to min_connections: {}", target_size);
        }

        if (m_live_connections.load(std::memory_order_acquire) <= target_size) {
            REDIS_LOG_INFO("[client]", "Current TLS size ({}) <= target size ({}), no shrink needed",
                         m_live_connections.load(std::memory_order_acquire), target_size);
            return 0;
        }

        size_t removed = 0;
        while (m_live_connections.load(std::memory_order_acquire) > target_size) {
            auto conn = tryAcquireAvailable();
            if (!conn) {
                break;
            }
            destroyConnectionSlot(conn);
            ++removed;
        }

        REDIS_LOG_INFO("[client]", "TLS pool shrink complete, removed {} connections, remaining: {}",
                     removed, m_live_connections.load(std::memory_order_acquire));
        return removed;
    }

    void RedissConnectionPool::shutdown()
    {
        if (m_is_shutting_down.exchange(true, std::memory_order_acq_rel)) {
            return;
        }

        REDIS_LOG_INFO("[client]", "Shutting down TLS connection pool");

        std::vector<std::shared_ptr<PooledRedissConnection>> idle_connections;
        std::vector<std::shared_ptr<detail::RedissPoolWaiter>> waiters_to_wake;

        const size_t drained_count = drainAvailableConnections(&idle_connections);

        std::shared_ptr<detail::RedissPoolWaiter> waiter;
        while (m_waiters.try_dequeue(waiter)) {
            if (waiter == nullptr) {
                continue;
            }
            if (detail::try_complete_waiter(waiter->state,
                                            detail::PoolWaiterState::Cancelled)) {
                waiters_to_wake.push_back(std::move(waiter));
            }
        }
        m_active_connections.store(0, std::memory_order_release);
        m_idle_connections.store(0, std::memory_order_release);
        m_live_connections.store(0, std::memory_order_release);

        for (auto& waiter : waiters_to_wake) {
            waiter->waker.wakeUp();
        }

        m_is_initialized.store(false, std::memory_order_release);
        REDIS_LOG_INFO("[client]", "TLS connection pool shutdown complete, closed {} connections",
                     drained_count);
    }

    RedissConnectionPool::PoolStats RedissConnectionPool::getStats() const
    {
        PoolStats stats;
        stats.total_connections = m_live_connections.load(std::memory_order_acquire);
        stats.available_connections = m_idle_connections.load(std::memory_order_acquire);
        stats.active_connections = m_active_connections.load(std::memory_order_acquire);
        stats.waiting_requests = m_waiting_requests.load();
        stats.total_acquired = m_total_acquired.load();
        stats.total_released = m_total_released.load();
        stats.total_created = m_total_created.load();
        stats.total_destroyed = m_total_destroyed.load();
        stats.health_check_failures = m_health_check_failures.load();
        stats.reconnect_attempts = m_reconnect_attempts.load();
        stats.reconnect_successes = m_reconnect_successes.load();
        stats.validation_failures = m_validation_failures.load();
        stats.total_acquire_time_ms = m_total_acquire_time_ms.load();
        stats.max_acquire_time_ms = m_max_acquire_time_ms.load();
        stats.peak_active_connections = m_peak_active_connections.load();

        if (stats.total_acquired > 0) {
            stats.avg_acquire_time_ms = static_cast<double>(stats.total_acquire_time_ms) / stats.total_acquired;
        } else {
            stats.avg_acquire_time_ms = 0.0;
        }

        return stats;
    }
#endif

} // namespace galay::redis
