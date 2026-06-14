#include "conn_pool.h"
#include "redis/base/redis_log.h"
#include <algorithm>

namespace galay::redis
{
    // ======================== PoolInitializeAwaitable 实现 ========================

    PoolInitializeAwaitable::PoolInitializeAwaitable(RedisConnectionPool& pool)
        : m_flow(std::make_unique<Flow>(pool))
        , m_inner(galay::kernel::AwaitableBuilder<Result, 4, Flow>(&m_controller, *m_flow)
                      .local<&Flow::run>()
                      .build())
    {
    }

    PoolInitializeAwaitable::Flow::Flow(RedisConnectionPool& pool)
        : m_pool(&pool)
    {
    }

    void PoolInitializeAwaitable::Flow::run(galay::kernel::SequenceOps<Result, 4>& ops)
    {
        ops.complete(m_pool->initializeSync());
    }

    // ======================== PoolAcquireAwaitable 实现 ========================

    PoolAcquireAwaitable::PoolAcquireAwaitable(RedisConnectionPool& pool)
        : m_flow(std::make_unique<Flow>(pool))
        , m_inner(galay::kernel::AwaitableBuilder<Result, 4, Flow>(&m_controller, *m_flow)
                      .local<&Flow::run>()
                      .build())
    {
    }

    PoolAcquireAwaitable::Flow::Flow(RedisConnectionPool& pool)
        : m_pool(&pool)
        , m_start_time(std::chrono::steady_clock::now())
    {
    }

    void PoolAcquireAwaitable::Flow::run(galay::kernel::SequenceOps<Result, 4>& ops)
    {
        ops.complete(m_pool->acquireSync(m_start_time));
    }

    // ======================== RedisConnectionPool 实现 ========================

    RedisConnectionPool::RedisConnectionPool(IOScheduler* scheduler, ConnectionPoolConfig config)
        : m_scheduler(scheduler)
        , m_config(std::move(config))
    {
        // 验证配置
        if (!m_config.validate()) {
            throw std::invalid_argument("Invalid connection pool configuration");
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

        size_t created_count = 0;
        while (created_count < m_config.initial_connections) {
            auto result = getConnectionSync();
            if (!result) {
                REDIS_LOG_ERROR("[client]", "Failed to create connection {}/{}: {}",
                              created_count + 1, m_config.initial_connections,
                              result.error().message());
                break;
            }

            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_available_connections.push(result.value());
            }
            ++created_count;
        }

        if (created_count < m_config.min_connections) {
            return std::unexpected(RedisError(
                RedisErrorType::REDIS_ERROR_TYPE_CONNECTION_ERROR,
                "Failed to create minimum connections"
            ));
        }

        m_is_initialized = true;
        REDIS_LOG_INFO("[client]", "Connection pool initialized with {} connections", created_count);
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
                    counter->fetch_sub(1);
                }
            }
        };

        m_waiting_requests.fetch_add(1);
        WaitingGuard waiting_guard{&m_waiting_requests};

        std::unique_lock<std::mutex> lock(m_mutex);

        while (!m_available_connections.empty()) {
            auto conn = m_available_connections.front();
            m_available_connections.pop();

            if (!conn->isClosed() && conn->isHealthy()) {
                conn->updateLastUsed();
                m_total_acquired++;
                lock.unlock();
                recordAcquireStats(start_time);
                return conn;
            }

            auto it = std::find(m_all_connections.begin(), m_all_connections.end(), conn);
            if (it != m_all_connections.end()) {
                m_all_connections.erase(it);
            }
            m_total_destroyed++;
        }

        const bool can_create = m_all_connections.size() < m_config.max_connections;
        lock.unlock();

        if (can_create) {
            auto result = getConnectionSync();
            if (result) {
                auto conn = result.value();
                conn->updateLastUsed();
                m_total_acquired++;
                recordAcquireStats(start_time);

                size_t total_connections = 0;
                {
                    std::lock_guard<std::mutex> stats_lock(m_mutex);
                    total_connections = m_all_connections.size();
                }
                REDIS_LOG_DEBUG("[client]", "Created and acquired new connection, total: {}", total_connections);
                return conn;
            }

            REDIS_LOG_WARN("[client]", "Failed to create new connection: {}", result.error().message());
            return std::unexpected(result.error());
        }

        return std::unexpected(RedisError(
            RedisErrorType::REDIS_ERROR_TYPE_TIMEOUT_ERROR,
            "No available connections"
        ));
    }

    void RedisConnectionPool::recordAcquireStats(std::chrono::steady_clock::time_point start_time)
    {
        const auto elapsed = std::chrono::steady_clock::now() - start_time;
        const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
        m_total_acquire_time_ms += elapsed_ms;

        double current_max = m_max_acquire_time_ms.load();
        while (elapsed_ms > current_max) {
            if (m_max_acquire_time_ms.compare_exchange_weak(current_max, elapsed_ms)) {
                break;
            }
        }

        size_t active = 0;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            active = m_all_connections.size() - m_available_connections.size();
        }
        size_t current_peak = m_peak_active_connections.load();
        while (active > current_peak) {
            if (m_peak_active_connections.compare_exchange_weak(current_peak, active)) {
                break;
            }
        }
    }

    void RedisConnectionPool::release(std::shared_ptr<PooledConnection> conn)
    {
        if (!conn) {
            return;
        }

        if (m_is_shutting_down) {
            REDIS_LOG_DEBUG("[client]", "Connection released during shutdown, will be destroyed");
            return;
        }

        std::lock_guard<std::mutex> lock(m_mutex);

        // 检查连接是否健康
        if (conn->isClosed() || !conn->isHealthy()) {
            REDIS_LOG_WARN("[client]", "Unhealthy connection released, removing from pool");
            auto it = std::find(m_all_connections.begin(), m_all_connections.end(), conn);
            if (it != m_all_connections.end()) {
                m_all_connections.erase(it);
            }
            m_total_destroyed++;
            return;
        }

        // 如果连接数超过最大值，销毁连接
        if (m_all_connections.size() > m_config.max_connections) {
            REDIS_LOG_DEBUG("[client]", "Pool size exceeds max, destroying connection");
            auto it = std::find(m_all_connections.begin(), m_all_connections.end(), conn);
            if (it != m_all_connections.end()) {
                m_all_connections.erase(it);
            }
            m_total_destroyed++;
            return;
        }

        // 归还到可用连接池
        m_available_connections.push(conn);
        m_total_released++;

        REDIS_LOG_DEBUG("[client]", "Connection released to pool, available: {}, total: {}",
                     m_available_connections.size(), m_all_connections.size());

        m_cv.notify_one();
    }

    std::expected<std::shared_ptr<PooledConnection>, RedisError>
    RedisConnectionPool::getConnectionSync()
    {
        REDIS_LOG_DEBUG("[client]", "Creating new connection to {}:{}", m_config.host, m_config.port);

        // 带重试的连接创建
        for (int attempt = 0; attempt < m_config.max_reconnect_attempts; ++attempt) {
            if (attempt > 0) {
                m_reconnect_attempts++;
                REDIS_LOG_INFO("[client]", "Reconnect attempt {}/{} for {}:{}",
                            attempt + 1, m_config.max_reconnect_attempts,
                            m_config.host, m_config.port);
            }

            try {
                auto client = std::make_shared<RedisClient>(m_scheduler);
                auto conn = std::make_shared<PooledConnection>(client, m_scheduler);

                // 注意：这里需要在协程上下文中调用 co_await client->connect()
                // 由于当前是同步方法，暂时创建未连接的客户端
                // 实际使用时，连接会在第一次使用时建立

                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    m_all_connections.push_back(conn);
                    m_total_created++;
                }

                if (attempt > 0) {
                    m_reconnect_successes++;
                    REDIS_LOG_INFO("[client]", "Reconnect succeeded on attempt {}", attempt + 1);
                }

                REDIS_LOG_DEBUG("[client]", "Connection created successfully, total: {}",
                             m_all_connections.size());
                return conn;

            } catch (const std::exception& e) {
                REDIS_LOG_ERROR("[client]", "Failed to create connection (attempt {}): {}",
                             attempt + 1, e.what());

                if (attempt == m_config.max_reconnect_attempts - 1) {
                    return std::unexpected(RedisError(
                        RedisErrorType::REDIS_ERROR_TYPE_CONNECTION_ERROR,
                        std::string("Failed to create connection after ") +
                        std::to_string(m_config.max_reconnect_attempts) + " attempts: " + e.what()
                    ));
                }
            }
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

        REDIS_LOG_INFO("[client]", "Running health check on {} connections", m_all_connections.size());

        std::vector<std::shared_ptr<PooledConnection>> unhealthy_connections;

        {
            std::lock_guard<std::mutex> lock(m_mutex);

            for (auto& conn : m_all_connections) {
                if (!checkConnectionHealthSync(conn)) {
                    unhealthy_connections.push_back(conn);
                }
            }

            if (!unhealthy_connections.empty()) {
                // 从可用连接中移除
                std::queue<std::shared_ptr<PooledConnection>> temp_queue;
                while (!m_available_connections.empty()) {
                    auto c = m_available_connections.front();
                    m_available_connections.pop();
                    if (c->isHealthy()) {
                        temp_queue.push(c);
                    }
                }
                m_available_connections = std::move(temp_queue);

                // 从所有连接中移除
                m_all_connections.erase(
                    std::remove_if(m_all_connections.begin(), m_all_connections.end(),
                        [](const auto& conn) { return !conn->isHealthy(); }),
                    m_all_connections.end()
                );

                m_total_destroyed += unhealthy_connections.size();

                REDIS_LOG_WARN("[client]", "Removed {} unhealthy connections, remaining: {}",
                            unhealthy_connections.size(), m_all_connections.size());
            }
        }

        // 如果连接数低于最小值，创建新连接
        size_t current_size = m_all_connections.size();
        while (current_size < m_config.min_connections) {
            auto result = getConnectionSync();
            if (!result) {
                REDIS_LOG_ERROR("[client]", "Failed to create replacement connection: {}",
                             result.error().message());
                break;
            }

            // 将新连接加入可用队列
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_available_connections.push(result.value());
                current_size = m_all_connections.size();
            }

            REDIS_LOG_INFO("[client]", "Created replacement connection, total: {}", current_size);
        }
    }

    void RedisConnectionPool::triggerIdleCleanup()
    {
        REDIS_LOG_INFO("[client]", "Running idle connection cleanup");

        std::vector<std::shared_ptr<PooledConnection>> idle_connections;

        {
            std::lock_guard<std::mutex> lock(m_mutex);

            // 检查可用连接中的空闲连接
            std::queue<std::shared_ptr<PooledConnection>> temp_queue;
            while (!m_available_connections.empty()) {
                auto conn = m_available_connections.front();
                m_available_connections.pop();

                if (conn->getIdleTime() > m_config.idle_timeout &&
                    m_all_connections.size() > m_config.min_connections) {
                    idle_connections.push_back(conn);
                } else {
                    temp_queue.push(conn);
                }
            }
            m_available_connections = std::move(temp_queue);
        }

        // 移除空闲连接
        if (!idle_connections.empty()) {
            std::lock_guard<std::mutex> lock(m_mutex);
            for (auto& conn : idle_connections) {
                auto it = std::find(m_all_connections.begin(), m_all_connections.end(), conn);
                if (it != m_all_connections.end()) {
                    m_all_connections.erase(it);
                    m_total_destroyed++;
                }
            }

            REDIS_LOG_INFO("[client]", "Cleaned up {} idle connections, remaining: {}",
                        idle_connections.size(), m_all_connections.size());
        }
    }

    void RedisConnectionPool::warmup()
    {
        REDIS_LOG_INFO("[client]", "Warming up connection pool to {} connections", m_config.min_connections);

        size_t current_size;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            current_size = m_all_connections.size();
        }

        size_t created = 0;
        while (current_size < m_config.min_connections) {
            auto result = getConnectionSync();
            if (!result) {
                REDIS_LOG_ERROR("[client]", "Failed to create warmup connection: {}", result.error().message());
                break;
            }

            // 将新连接加入可用队列
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_available_connections.push(result.value());
                current_size = m_all_connections.size();
            }
            created++;
        }

        REDIS_LOG_INFO("[client]", "Warmup complete, created {} connections, total: {}", created, current_size);
    }

    size_t RedisConnectionPool::cleanupUnhealthyConnections()
    {
        REDIS_LOG_INFO("[client]", "Cleaning up unhealthy connections");

        std::vector<std::shared_ptr<PooledConnection>> unhealthy_connections;

        {
            std::lock_guard<std::mutex> lock(m_mutex);

            // 找出所有不健康的连接
            for (auto& conn : m_all_connections) {
                if (conn->isClosed() || !conn->isHealthy()) {
                    unhealthy_connections.push_back(conn);
                }
            }

            if (!unhealthy_connections.empty()) {
                // 从可用连接中移除
                std::queue<std::shared_ptr<PooledConnection>> temp_queue;
                while (!m_available_connections.empty()) {
                    auto c = m_available_connections.front();
                    m_available_connections.pop();
                    if (!c->isClosed() && c->isHealthy()) {
                        temp_queue.push(c);
                    }
                }
                m_available_connections = std::move(temp_queue);

                // 从所有连接中移除
                m_all_connections.erase(
                    std::remove_if(m_all_connections.begin(), m_all_connections.end(),
                        [](const auto& conn) { return conn->isClosed() || !conn->isHealthy(); }),
                    m_all_connections.end()
                );

                m_total_destroyed += unhealthy_connections.size();
            }
        }

        size_t removed = unhealthy_connections.size();
        if (removed > 0) {
            REDIS_LOG_INFO("[client]", "Cleaned up {} unhealthy connections, remaining: {}",
                        removed, m_all_connections.size());
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
            size_t current_size;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                current_size = m_all_connections.size();
            }

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

            // 将新连接加入可用队列
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_available_connections.push(result.value());
            }
            created++;
        }

        REDIS_LOG_INFO("[client]", "Pool expansion complete, created {} connections, total: {}",
                     created, m_all_connections.size());
        return created;
    }

    size_t RedisConnectionPool::shrinkPool(size_t target_size)
    {
        REDIS_LOG_INFO("[client]", "Shrinking pool to {} connections", target_size);

        std::vector<std::shared_ptr<PooledConnection>> connections_to_remove;

        {
            std::lock_guard<std::mutex> lock(m_mutex);

            // 确保不低于最小连接数
            if (target_size < m_config.min_connections) {
                target_size = m_config.min_connections;
                REDIS_LOG_WARN("[client]", "Target size adjusted to min_connections: {}", target_size);
            }

            // 如果当前连接数已经小于等于目标，不需要缩容
            if (m_all_connections.size() <= target_size) {
                REDIS_LOG_INFO("[client]", "Current size ({}) <= target size ({}), no shrink needed",
                            m_all_connections.size(), target_size);
                return 0;
            }

            // 从可用连接中移除多余的连接
            size_t to_remove = m_all_connections.size() - target_size;
            std::queue<std::shared_ptr<PooledConnection>> temp_queue;

            while (!m_available_connections.empty() && connections_to_remove.size() < to_remove) {
                auto conn = m_available_connections.front();
                m_available_connections.pop();
                connections_to_remove.push_back(conn);
            }

            // 将剩余的连接放回队列
            while (!m_available_connections.empty()) {
                temp_queue.push(m_available_connections.front());
                m_available_connections.pop();
            }
            m_available_connections = std::move(temp_queue);

            // 从所有连接中移除
            for (auto& conn : connections_to_remove) {
                auto it = std::find(m_all_connections.begin(), m_all_connections.end(), conn);
                if (it != m_all_connections.end()) {
                    m_all_connections.erase(it);
                }
            }

            m_total_destroyed += connections_to_remove.size();
        }

        size_t removed = connections_to_remove.size();
        REDIS_LOG_INFO("[client]", "Pool shrink complete, removed {} connections, remaining: {}",
                     removed, m_all_connections.size());
        return removed;
    }

    void RedisConnectionPool::shutdown()
    {
        if (m_is_shutting_down) {
            return;
        }

        m_is_shutting_down = true;
        REDIS_LOG_INFO("[client]", "Shutting down connection pool");

        std::vector<std::shared_ptr<PooledConnection>> all_connections;

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            all_connections = m_all_connections;
            m_all_connections.clear();

            // 清空可用连接队列
            while (!m_available_connections.empty()) {
                m_available_connections.pop();
            }
        }

        m_is_initialized = false;
        REDIS_LOG_INFO("[client]", "Connection pool shutdown complete, closed {} connections",
                     all_connections.size());
    }

    RedisConnectionPool::PoolStats RedisConnectionPool::getStats() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        PoolStats stats;
        stats.total_connections = m_all_connections.size();
        stats.available_connections = m_available_connections.size();
        stats.active_connections = stats.total_connections - stats.available_connections;
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
    RedissPoolInitializeAwaitable::RedissPoolInitializeAwaitable(RedissConnectionPool& pool)
        : m_flow(std::make_unique<Flow>(pool))
        , m_inner(galay::kernel::AwaitableBuilder<Result, 4, Flow>(&m_controller, *m_flow)
                      .local<&Flow::run>()
                      .build())
    {
    }

    RedissPoolInitializeAwaitable::Flow::Flow(RedissConnectionPool& pool)
        : m_pool(&pool)
    {
    }

    void RedissPoolInitializeAwaitable::Flow::run(galay::kernel::SequenceOps<Result, 4>& ops)
    {
        ops.complete(m_pool->initializeSync());
    }

    RedissPoolAcquireAwaitable::RedissPoolAcquireAwaitable(RedissConnectionPool& pool)
        : m_flow(std::make_unique<Flow>(pool))
        , m_inner(galay::kernel::AwaitableBuilder<Result, 4, Flow>(&m_controller, *m_flow)
                      .local<&Flow::run>()
                      .build())
    {
    }

    RedissPoolAcquireAwaitable::Flow::Flow(RedissConnectionPool& pool)
        : m_pool(&pool)
        , m_start_time(std::chrono::steady_clock::now())
    {
    }

    void RedissPoolAcquireAwaitable::Flow::run(galay::kernel::SequenceOps<Result, 4>& ops)
    {
        ops.complete(m_pool->acquireSync(m_start_time));
    }

    RedissConnectionPool::RedissConnectionPool(IOScheduler* scheduler, RedissConnectionPoolConfig config)
        : m_scheduler(scheduler)
        , m_config(std::move(config))
    {
        if (!m_config.validate()) {
            throw std::invalid_argument("Invalid TLS connection pool configuration");
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

            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_available_connections.push(result.value());
            }
            ++created_count;
        }

        if (created_count < m_config.min_connections) {
            return std::unexpected(RedisError(
                RedisErrorType::REDIS_ERROR_TYPE_CONNECTION_ERROR,
                "Failed to create minimum TLS connections"
            ));
        }

        m_is_initialized = true;
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
                    counter->fetch_sub(1);
                }
            }
        };

        m_waiting_requests.fetch_add(1);
        WaitingGuard waiting_guard{&m_waiting_requests};

        std::unique_lock<std::mutex> lock(m_mutex);

        while (!m_available_connections.empty()) {
            auto conn = m_available_connections.front();
            m_available_connections.pop();

            if (!conn->isClosed() && conn->isHealthy()) {
                conn->updateLastUsed();
                m_total_acquired++;
                lock.unlock();
                recordAcquireStats(start_time);
                return conn;
            }

            auto it = std::find(m_all_connections.begin(), m_all_connections.end(), conn);
            if (it != m_all_connections.end()) {
                m_all_connections.erase(it);
            }
            m_total_destroyed++;
        }

        const bool can_create = m_all_connections.size() < m_config.max_connections;
        lock.unlock();

        if (can_create) {
            auto result = getConnectionSync();
            if (result) {
                auto conn = result.value();
                conn->updateLastUsed();
                m_total_acquired++;
                recordAcquireStats(start_time);

                size_t total_connections = 0;
                {
                    std::lock_guard<std::mutex> stats_lock(m_mutex);
                    total_connections = m_all_connections.size();
                }
                REDIS_LOG_DEBUG("[client]", "Created and acquired new TLS connection, total: {}", total_connections);
                return conn;
            }

            REDIS_LOG_WARN("[client]", "Failed to create new TLS connection: {}", result.error().message());
            return std::unexpected(result.error());
        }

        return std::unexpected(RedisError(
            RedisErrorType::REDIS_ERROR_TYPE_TIMEOUT_ERROR,
            "No available TLS connections"
        ));
    }

    void RedissConnectionPool::recordAcquireStats(std::chrono::steady_clock::time_point start_time)
    {
        const auto elapsed = std::chrono::steady_clock::now() - start_time;
        const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
        m_total_acquire_time_ms += elapsed_ms;

        double current_max = m_max_acquire_time_ms.load();
        while (elapsed_ms > current_max) {
            if (m_max_acquire_time_ms.compare_exchange_weak(current_max, elapsed_ms)) {
                break;
            }
        }

        size_t active = 0;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            active = m_all_connections.size() - m_available_connections.size();
        }
        size_t current_peak = m_peak_active_connections.load();
        while (active > current_peak) {
            if (m_peak_active_connections.compare_exchange_weak(current_peak, active)) {
                break;
            }
        }
    }

    void RedissConnectionPool::release(std::shared_ptr<PooledRedissConnection> conn)
    {
        if (!conn) {
            return;
        }

        if (m_is_shutting_down) {
            REDIS_LOG_DEBUG("[client]", "TLS connection released during shutdown, will be destroyed");
            return;
        }

        std::lock_guard<std::mutex> lock(m_mutex);

        if (conn->isClosed() || !conn->isHealthy()) {
            REDIS_LOG_WARN("[client]", "Unhealthy TLS connection released, removing from pool");
            auto it = std::find(m_all_connections.begin(), m_all_connections.end(), conn);
            if (it != m_all_connections.end()) {
                m_all_connections.erase(it);
            }
            m_total_destroyed++;
            return;
        }

        if (m_all_connections.size() > m_config.max_connections) {
            REDIS_LOG_DEBUG("[client]", "TLS pool size exceeds max, destroying connection");
            auto it = std::find(m_all_connections.begin(), m_all_connections.end(), conn);
            if (it != m_all_connections.end()) {
                m_all_connections.erase(it);
            }
            m_total_destroyed++;
            return;
        }

        m_available_connections.push(conn);
        m_total_released++;

        REDIS_LOG_DEBUG("[client]", "TLS connection released to pool, available: {}, total: {}",
                      m_available_connections.size(), m_all_connections.size());

        m_cv.notify_one();
    }

    std::expected<std::shared_ptr<PooledRedissConnection>, RedisError>
    RedissConnectionPool::getConnectionSync()
    {
        REDIS_LOG_DEBUG("[client]", "Creating new TLS connection to {}:{}", m_config.host, m_config.port);

        for (int attempt = 0; attempt < m_config.max_reconnect_attempts; ++attempt) {
            if (attempt > 0) {
                m_reconnect_attempts++;
                REDIS_LOG_INFO("[client]", "TLS reconnect attempt {}/{} for {}:{}",
                             attempt + 1, m_config.max_reconnect_attempts,
                             m_config.host, m_config.port);
            }

            try {
                AsyncRedisConfig async_config;
                async_config.send_timeout = m_config.connect_timeout;
                async_config.recv_timeout = m_config.connect_timeout;

                auto client = std::make_shared<RedissClient>(
                    m_scheduler,
                    async_config,
                    m_config.tls_config);
                auto conn = std::make_shared<PooledRedissConnection>(client, m_scheduler);

                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    m_all_connections.push_back(conn);
                    m_total_created++;
                }

                if (attempt > 0) {
                    m_reconnect_successes++;
                    REDIS_LOG_INFO("[client]", "TLS reconnect succeeded on attempt {}", attempt + 1);
                }

                REDIS_LOG_DEBUG("[client]", "TLS connection created successfully, total: {}",
                              m_all_connections.size());
                return conn;

            } catch (const std::exception& e) {
                REDIS_LOG_ERROR("[client]", "Failed to create TLS connection (attempt {}): {}",
                              attempt + 1, e.what());

                if (attempt == m_config.max_reconnect_attempts - 1) {
                    return std::unexpected(RedisError(
                        RedisErrorType::REDIS_ERROR_TYPE_CONNECTION_ERROR,
                        std::string("Failed to create TLS connection after ") +
                        std::to_string(m_config.max_reconnect_attempts) + " attempts: " + e.what()
                    ));
                }
            }
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

        REDIS_LOG_INFO("[client]", "Running TLS health check on {} connections", m_all_connections.size());

        std::vector<std::shared_ptr<PooledRedissConnection>> unhealthy_connections;

        {
            std::lock_guard<std::mutex> lock(m_mutex);

            for (auto& conn : m_all_connections) {
                if (!checkConnectionHealthSync(conn)) {
                    unhealthy_connections.push_back(conn);
                }
            }

            if (!unhealthy_connections.empty()) {
                std::queue<std::shared_ptr<PooledRedissConnection>> temp_queue;
                while (!m_available_connections.empty()) {
                    auto c = m_available_connections.front();
                    m_available_connections.pop();
                    if (c->isHealthy()) {
                        temp_queue.push(c);
                    }
                }
                m_available_connections = std::move(temp_queue);

                m_all_connections.erase(
                    std::remove_if(m_all_connections.begin(), m_all_connections.end(),
                        [](const auto& conn) { return !conn->isHealthy(); }),
                    m_all_connections.end()
                );

                m_total_destroyed += unhealthy_connections.size();

                REDIS_LOG_WARN("[client]", "Removed {} unhealthy TLS connections, remaining: {}",
                             unhealthy_connections.size(), m_all_connections.size());
            }
        }

        size_t current_size = m_all_connections.size();
        while (current_size < m_config.min_connections) {
            auto result = getConnectionSync();
            if (!result) {
                REDIS_LOG_ERROR("[client]", "Failed to create replacement TLS connection: {}",
                              result.error().message());
                break;
            }

            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_available_connections.push(result.value());
                current_size = m_all_connections.size();
            }

            REDIS_LOG_INFO("[client]", "Created replacement TLS connection, total: {}", current_size);
        }
    }

    void RedissConnectionPool::triggerIdleCleanup()
    {
        REDIS_LOG_INFO("[client]", "Running TLS idle connection cleanup");

        std::vector<std::shared_ptr<PooledRedissConnection>> idle_connections;

        {
            std::lock_guard<std::mutex> lock(m_mutex);

            std::queue<std::shared_ptr<PooledRedissConnection>> temp_queue;
            while (!m_available_connections.empty()) {
                auto conn = m_available_connections.front();
                m_available_connections.pop();

                if (conn->getIdleTime() > m_config.idle_timeout &&
                    m_all_connections.size() > m_config.min_connections) {
                    idle_connections.push_back(conn);
                } else {
                    temp_queue.push(conn);
                }
            }
            m_available_connections = std::move(temp_queue);
        }

        if (!idle_connections.empty()) {
            std::lock_guard<std::mutex> lock(m_mutex);
            for (auto& conn : idle_connections) {
                auto it = std::find(m_all_connections.begin(), m_all_connections.end(), conn);
                if (it != m_all_connections.end()) {
                    m_all_connections.erase(it);
                    m_total_destroyed++;
                }
            }

            REDIS_LOG_INFO("[client]", "Cleaned up {} idle TLS connections, remaining: {}",
                         idle_connections.size(), m_all_connections.size());
        }
    }

    void RedissConnectionPool::warmup()
    {
        REDIS_LOG_INFO("[client]", "Warming up TLS connection pool to {} connections", m_config.min_connections);

        size_t current_size;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            current_size = m_all_connections.size();
        }

        size_t created = 0;
        while (current_size < m_config.min_connections) {
            auto result = getConnectionSync();
            if (!result) {
                REDIS_LOG_ERROR("[client]", "Failed to create warmup TLS connection: {}",
                              result.error().message());
                break;
            }

            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_available_connections.push(result.value());
                current_size = m_all_connections.size();
            }
            created++;
        }

        REDIS_LOG_INFO("[client]", "TLS warmup complete, created {} connections, total: {}", created, current_size);
    }

    size_t RedissConnectionPool::cleanupUnhealthyConnections()
    {
        REDIS_LOG_INFO("[client]", "Cleaning up unhealthy TLS connections");

        std::vector<std::shared_ptr<PooledRedissConnection>> unhealthy_connections;

        {
            std::lock_guard<std::mutex> lock(m_mutex);

            for (auto& conn : m_all_connections) {
                if (conn->isClosed() || !conn->isHealthy()) {
                    unhealthy_connections.push_back(conn);
                }
            }

            if (!unhealthy_connections.empty()) {
                std::queue<std::shared_ptr<PooledRedissConnection>> temp_queue;
                while (!m_available_connections.empty()) {
                    auto c = m_available_connections.front();
                    m_available_connections.pop();
                    if (!c->isClosed() && c->isHealthy()) {
                        temp_queue.push(c);
                    }
                }
                m_available_connections = std::move(temp_queue);

                m_all_connections.erase(
                    std::remove_if(m_all_connections.begin(), m_all_connections.end(),
                        [](const auto& conn) { return conn->isClosed() || !conn->isHealthy(); }),
                    m_all_connections.end()
                );

                m_total_destroyed += unhealthy_connections.size();
            }
        }

        size_t removed = unhealthy_connections.size();
        if (removed > 0) {
            REDIS_LOG_INFO("[client]", "Cleaned up {} unhealthy TLS connections, remaining: {}",
                         removed, m_all_connections.size());
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
            size_t current_size;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                current_size = m_all_connections.size();
            }

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

            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_available_connections.push(result.value());
            }
            created++;
        }

        REDIS_LOG_INFO("[client]", "TLS pool expansion complete, created {} connections, total: {}",
                     created, m_all_connections.size());
        return created;
    }

    size_t RedissConnectionPool::shrinkPool(size_t target_size)
    {
        REDIS_LOG_INFO("[client]", "Shrinking TLS pool to {} connections", target_size);

        std::vector<std::shared_ptr<PooledRedissConnection>> connections_to_remove;

        {
            std::lock_guard<std::mutex> lock(m_mutex);

            if (target_size < m_config.min_connections) {
                target_size = m_config.min_connections;
                REDIS_LOG_WARN("[client]", "TLS target size adjusted to min_connections: {}", target_size);
            }

            if (m_all_connections.size() <= target_size) {
                REDIS_LOG_INFO("[client]", "Current TLS size ({}) <= target size ({}), no shrink needed",
                             m_all_connections.size(), target_size);
                return 0;
            }

            size_t to_remove = m_all_connections.size() - target_size;
            std::queue<std::shared_ptr<PooledRedissConnection>> temp_queue;

            while (!m_available_connections.empty() && connections_to_remove.size() < to_remove) {
                auto conn = m_available_connections.front();
                m_available_connections.pop();
                connections_to_remove.push_back(conn);
            }

            while (!m_available_connections.empty()) {
                temp_queue.push(m_available_connections.front());
                m_available_connections.pop();
            }
            m_available_connections = std::move(temp_queue);

            for (auto& conn : connections_to_remove) {
                auto it = std::find(m_all_connections.begin(), m_all_connections.end(), conn);
                if (it != m_all_connections.end()) {
                    m_all_connections.erase(it);
                }
            }

            m_total_destroyed += connections_to_remove.size();
        }

        size_t removed = connections_to_remove.size();
        REDIS_LOG_INFO("[client]", "TLS pool shrink complete, removed {} connections, remaining: {}",
                     removed, m_all_connections.size());
        return removed;
    }

    void RedissConnectionPool::shutdown()
    {
        if (m_is_shutting_down) {
            return;
        }

        m_is_shutting_down = true;
        REDIS_LOG_INFO("[client]", "Shutting down TLS connection pool");

        std::vector<std::shared_ptr<PooledRedissConnection>> all_connections;

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            all_connections = m_all_connections;
            m_all_connections.clear();

            while (!m_available_connections.empty()) {
                m_available_connections.pop();
            }
        }

        m_is_initialized = false;
        REDIS_LOG_INFO("[client]", "TLS connection pool shutdown complete, closed {} connections",
                     all_connections.size());
    }

    RedissConnectionPool::PoolStats RedissConnectionPool::getStats() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);

        PoolStats stats;
        stats.total_connections = m_all_connections.size();
        stats.available_connections = m_available_connections.size();
        stats.active_connections = stats.total_connections - stats.available_connections;
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
