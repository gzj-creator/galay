/**
 * @file conn_pool.h
 * @brief Redis 连接池管理
 * @author galay-redis
 * @version 1.0.0
 *
 * @details 提供 Redis/Rediss 连接池的配置、连接包装、协程等待体以及连接池实现，
 *          支持自动扩缩容、健康检查、空闲清理和 RAII 风格的连接管理。
 */

#ifndef GALAY_REDIS_CONNECTION_POOL_H
#define GALAY_REDIS_CONNECTION_POOL_H

#include "redis_client.h"
#include "conn_pool_waiter_state.h"
#include "../../galay-kernel/core/awaitable.h"
#include "../../galay-kernel/core/io_scheduler.hpp"
#include "../../galay-kernel/core/waker.h"
#include <memory>
#include <queue>
#include <vector>
#include <mutex>
#include <atomic>
#include <chrono>
#include <coroutine>
#include <optional>

namespace galay::redis
{
    using galay::kernel::IOScheduler;

    /**
     * @brief Redis 连接池配置
     * @details 包含连接参数、连接池大小、超时配置、健康检查和重连等配置项
     */
    struct ConnectionPoolConfig
    {
        // 连接参数
        std::string host = "127.0.0.1";        ///< Redis 服务器地址
        std::string username = "";              ///< 认证用户名
        std::string password = "";              ///< 认证密码

        // 连接池大小
        size_t min_connections = 2;      ///< 最小连接数
        size_t max_connections = 10;     ///< 最大连接数
        size_t initial_connections = 2;  ///< 初始连接数

        // 超时配置
        std::chrono::milliseconds acquire_timeout = std::chrono::seconds(5);  ///< 获取连接超时
        std::chrono::milliseconds idle_timeout = std::chrono::minutes(5);     ///< 空闲连接超时
        std::chrono::milliseconds connect_timeout = std::chrono::seconds(3);  ///< 连接超时

        // 健康检查
        std::chrono::milliseconds health_check_interval = std::chrono::seconds(30); ///< 健康检查间隔

        int32_t port = 6379;                    ///< Redis 服务器端口
        int32_t db_index = 0;                   ///< 数据库索引

        // 重连配置
        int max_reconnect_attempts = 3;     ///< 最大重连尝试次数

        bool enable_health_check = true;         ///< 是否启用健康检查
        bool enable_auto_reconnect = true;       ///< 是否启用自动重连

        // 连接验证配置
        bool enable_connection_validation = true;  ///< 获取连接时是否验证
        bool validate_on_acquire = false;          ///< 每次获取时都验证（性能开销较大）
        bool validate_on_return = false;           ///< 归还时验证

        /**
         * @brief 验证配置参数是否合法
         * @return 配置合法返回 true
         */
        bool validate() const
        {
            return min_connections <= max_connections &&
                   initial_connections >= min_connections &&
                   initial_connections <= max_connections &&
                   max_connections > 0;
        }

        /**
         * @brief 创建默认配置
         * @return 默认连接池配置
         */
        static ConnectionPoolConfig defaultConfig()
        {
            return ConnectionPoolConfig{};
        }

        /**
         * @brief 创建自定义配置
         * @param host Redis 服务器地址
         * @param port Redis 服务器端口
         * @param min_conn 最小连接数
         * @param max_conn 最大连接数
         * @return 自定义连接池配置
         */
        static ConnectionPoolConfig create(const std::string& host, int32_t port,
                                          size_t min_conn = 2, size_t max_conn = 10)
        {
            ConnectionPoolConfig config;
            config.host = host;
            config.port = port;
            config.min_connections = min_conn;
            config.max_connections = max_conn;
            config.initial_connections = min_conn;
            return config;
        }
    };

#ifdef GALAY_SSL_FEATURE_ENABLED
    /**
     * @brief Rediss（TLS）连接池配置
     * @details 与 ConnectionPoolConfig 类似，但增加了 TLS 配置项，默认端口为 6380
     */
    struct RedissConnectionPoolConfig
    {
        std::string host = "127.0.0.1";        ///< Redis 服务器地址
        std::string username = "";              ///< 认证用户名
        std::string password = "";              ///< 认证密码
        RedissClientConfig tls_config;           ///< TLS 配置

        size_t min_connections = 2;             ///< 最小连接数
        size_t max_connections = 10;            ///< 最大连接数
        size_t initial_connections = 2;         ///< 初始连接数

        std::chrono::milliseconds acquire_timeout = std::chrono::seconds(5);   ///< 获取连接超时
        std::chrono::milliseconds idle_timeout = std::chrono::minutes(5);      ///< 空闲连接超时
        std::chrono::milliseconds connect_timeout = std::chrono::seconds(3);   ///< 连接超时

        std::chrono::milliseconds health_check_interval = std::chrono::seconds(30); ///< 健康检查间隔

        int32_t port = 6380;                    ///< Redis TLS 端口
        int32_t db_index = 0;                   ///< 数据库索引
        int max_reconnect_attempts = 3;     ///< 最大重连尝试次数

        bool enable_health_check = true;     ///< 是否启用健康检查
        bool enable_auto_reconnect = true;   ///< 是否启用自动重连
        bool enable_connection_validation = true;  ///< 获取连接时是否验证
        bool validate_on_acquire = false;          ///< 每次获取时都验证
        bool validate_on_return = false;           ///< 归还时验证

        /**
         * @brief 验证配置参数是否合法
         * @return 配置合法返回 true
         */
        bool validate() const
        {
            return min_connections <= max_connections &&
                   initial_connections >= min_connections &&
                   initial_connections <= max_connections &&
                   max_connections > 0;
        }

        /**
         * @brief 创建默认配置
         * @return 默认 Rediss 连接池配置
         */
        static RedissConnectionPoolConfig defaultConfig()
        {
            return RedissConnectionPoolConfig{};
        }

        /**
         * @brief 创建自定义配置
         * @param host Redis 服务器地址
         * @param port Redis 服务器端口
         * @param min_conn 最小连接数
         * @param max_conn 最大连接数
         * @return 自定义 Rediss 连接池配置
         */
        static RedissConnectionPoolConfig create(const std::string& host, int32_t port,
                                                 size_t min_conn = 2, size_t max_conn = 10)
        {
            RedissConnectionPoolConfig config;
            config.host = host;
            config.port = port;
            config.min_connections = min_conn;
            config.max_connections = max_conn;
            config.initial_connections = min_conn;
            return config;
        }
    };
#endif

    /**
     * @brief Redis 连接包装器，用于管理连接的生命周期
     * @details 封装 RedisClient 指针，提供最后使用时间、健康状态等管理功能
     */
    class PooledConnection
    {
    public:
        /**
         * @brief 构造连接包装器
         * @param client Redis 客户端智能指针
         * @param scheduler IO 调度器指针
         */
        PooledConnection(std::shared_ptr<RedisClient<>> client, IOScheduler* scheduler)
            : m_client(std::move(client))
            , m_scheduler(scheduler)
            , m_last_used(std::chrono::steady_clock::now())
            , m_is_healthy(false)
        {
        }

        RedisClient<>* get() { return m_client.get(); }              ///< 获取原始客户端指针
        const RedisClient<>* get() const { return m_client.get(); }  ///< 获取原始客户端指针（const）

        RedisClient<>* operator->() { return m_client.get(); }              ///< 箭头操作符访问客户端
        const RedisClient<>* operator->() const { return m_client.get(); }  ///< 箭头操作符访问客户端（const）

        RedisClient<>& operator*() { return *m_client; }              ///< 解引用操作符
        const RedisClient<>& operator*() const { return *m_client; }  ///< 解引用操作符（const）

        /**
         * @brief 更新最后使用时间为当前时刻
         */
        void updateLastUsed()
        {
            m_last_used = std::chrono::steady_clock::now();
        }

        /**
         * @brief 获取连接空闲时间
         * @return 自上次使用以来经过的毫秒数
         */
        std::chrono::milliseconds getIdleTime() const
        {
            auto now = std::chrono::steady_clock::now();
            return std::chrono::duration_cast<std::chrono::milliseconds>(now - m_last_used);
        }

        bool isHealthy() const { return m_is_healthy; }            ///< 获取健康状态
        void setHealthy(bool healthy) { m_is_healthy = healthy; }  ///< 设置健康状态
        bool isClosed() const { return m_client->isClosed(); }     ///< 检查连接是否已关闭

    private:
        std::shared_ptr<RedisClient<>> m_client;                          ///< 底层 Redis 客户端
        IOScheduler* m_scheduler;                                        ///< IO 调度器
        std::chrono::steady_clock::time_point m_last_used;               ///< 最后使用时间
        bool m_is_healthy;                                               ///< 健康状态标志
    };

#ifdef GALAY_SSL_FEATURE_ENABLED
    /**
     * @brief Rediss（TLS）连接包装器
     * @details 封装 RedissClient 指针，提供与 PooledConnection 相同的管理功能
     */
    class PooledRedissConnection
    {
    public:
        /**
         * @brief 构造 Rediss 连接包装器
         * @param client Rediss 客户端智能指针
         * @param scheduler IO 调度器指针
         */
        PooledRedissConnection(std::shared_ptr<RedissClient> client, IOScheduler* scheduler)
            : m_client(std::move(client))
            , m_scheduler(scheduler)
            , m_last_used(std::chrono::steady_clock::now())
            , m_is_healthy(false)
        {
        }

        RedissClient* get() { return m_client.get(); }              ///< 获取原始客户端指针
        const RedissClient* get() const { return m_client.get(); }  ///< 获取原始客户端指针（const）

        RedissClient* operator->() { return m_client.get(); }              ///< 箭头操作符访问客户端
        const RedissClient* operator->() const { return m_client.get(); }  ///< 箭头操作符访问客户端（const）

        RedissClient& operator*() { return *m_client; }              ///< 解引用操作符
        const RedissClient& operator*() const { return *m_client; }  ///< 解引用操作符（const）

        /**
         * @brief 更新最后使用时间为当前时刻
         */
        void updateLastUsed()
        {
            m_last_used = std::chrono::steady_clock::now();
        }

        /**
         * @brief 获取连接空闲时间
         * @return 自上次使用以来经过的毫秒数
         */
        std::chrono::milliseconds getIdleTime() const
        {
            auto now = std::chrono::steady_clock::now();
            return std::chrono::duration_cast<std::chrono::milliseconds>(now - m_last_used);
        }

        bool isHealthy() const { return m_is_healthy; }            ///< 获取健康状态
        void setHealthy(bool healthy) { m_is_healthy = healthy; }  ///< 设置健康状态
        bool isClosed() const { return m_client->isClosed(); }     ///< 检查连接是否已关闭

    private:
        std::shared_ptr<RedissClient> m_client;                         ///< 底层 Rediss 客户端
        IOScheduler* m_scheduler;                                       ///< IO 调度器
        std::chrono::steady_clock::time_point m_last_used;              ///< 最后使用时间
        bool m_is_healthy;                                              ///< 健康状态标志
    };
#endif

    namespace detail
    {
        struct RedisPoolWaiter
        {
            explicit RedisPoolWaiter(galay::kernel::Waker waiter_waker)
                : waker(std::move(waiter_waker))
            {
            }

            galay::kernel::Waker waker;
            std::shared_ptr<PooledConnection> connection;
            std::atomic<PoolWaiterState> state{PoolWaiterState::Waiting};
        };

#ifdef GALAY_SSL_FEATURE_ENABLED
        struct RedissPoolWaiter
        {
            explicit RedissPoolWaiter(galay::kernel::Waker waiter_waker)
                : waker(std::move(waiter_waker))
            {
            }

            galay::kernel::Waker waker;
            std::shared_ptr<PooledRedissConnection> connection;
            std::atomic<PoolWaiterState> state{PoolWaiterState::Waiting};
        };
#endif
    } // namespace detail

    // 前向声明
    class RedisConnectionPool;
#ifdef GALAY_SSL_FEATURE_ENABLED
    class RedissConnectionPool;
#endif

    /**
     * @brief 连接池初始化等待体
     */
    class PoolInitializeAwaitable : public galay::kernel::TimeoutSupport<PoolInitializeAwaitable>
    {
    public:
        using Result = RedisVoidResult;

        PoolInitializeAwaitable(RedisConnectionPool& pool);
        PoolInitializeAwaitable(const PoolInitializeAwaitable&) = delete;
        PoolInitializeAwaitable& operator=(const PoolInitializeAwaitable&) = delete;
        PoolInitializeAwaitable(PoolInitializeAwaitable&&) noexcept = default;
        PoolInitializeAwaitable& operator=(PoolInitializeAwaitable&&) noexcept = default;

        bool await_ready() const noexcept { return true; }
        template <typename Promise>
        bool await_suspend(std::coroutine_handle<Promise>)
        {
            return false;
        }
        Result await_resume() { return std::move(m_result); }
        void markTimeout() {}

    private:
        Result m_result;
    };

    /**
     * @brief 连接池获取连接等待体
     */
    class PoolAcquireAwaitable : public galay::kernel::TimeoutSupport<PoolAcquireAwaitable>
    {
    public:
        using Result = std::expected<std::shared_ptr<PooledConnection>, RedisError>;

        PoolAcquireAwaitable(RedisConnectionPool& pool);
        PoolAcquireAwaitable(const PoolAcquireAwaitable&) = delete;
        PoolAcquireAwaitable& operator=(const PoolAcquireAwaitable&) = delete;
        PoolAcquireAwaitable(PoolAcquireAwaitable&&) noexcept = default;
        PoolAcquireAwaitable& operator=(PoolAcquireAwaitable&&) noexcept = default;

        bool await_ready() const noexcept { return false; }
        template <typename Promise>
        requires requires(const Promise& promise) {
            { promise.taskRefView() } -> std::same_as<const galay::kernel::TaskRef&>;
        }
        bool await_suspend(std::coroutine_handle<Promise> handle)
        {
            const auto action = prepareSuspend(galay::kernel::Waker(handle));
            if (action == SuspendAction::Wait) {
                return true;
            }
            if (action == SuspendAction::Connect && m_connect_awaitable.has_value()) {
                return m_connect_awaitable->await_suspend(handle);
            }
            return false;
        }
        Result await_resume();
        void markTimeout();

    private:
        enum class SuspendAction {
            Ready,
            Connect,
            Wait,
            Error,
        };

        enum class State {
            Invalid,
            Ready,
            Creating,
            Waiting,
            EnqueueFailed,
            TimedOut,
            Error,
        };

        SuspendAction prepareSuspend(galay::kernel::Waker waiter_waker);

        std::optional<RedisConnectOperation> m_connect_awaitable;
        std::optional<RedisError> m_error;
        std::shared_ptr<PooledConnection> m_connection;
        std::shared_ptr<detail::RedisPoolWaiter> m_waiter;
        RedisConnectionPool* m_pool = nullptr;
        std::chrono::steady_clock::time_point m_start_time;
        State m_state = State::Invalid;
        bool m_wait_counted = false;
    };

#ifdef GALAY_SSL_FEATURE_ENABLED
    /**
     * @brief Rediss 连接池初始化等待体
     * @details 用于协程中等待 Rediss 连接池初始化完成
     */
    class RedissPoolInitializeAwaitable : public galay::kernel::TimeoutSupport<RedissPoolInitializeAwaitable>
    {
    public:
        using Result = RedisVoidResult;

        explicit RedissPoolInitializeAwaitable(RedissConnectionPool& pool);
        RedissPoolInitializeAwaitable(const RedissPoolInitializeAwaitable&) = delete;
        RedissPoolInitializeAwaitable& operator=(const RedissPoolInitializeAwaitable&) = delete;
        RedissPoolInitializeAwaitable(RedissPoolInitializeAwaitable&&) noexcept = default;
        RedissPoolInitializeAwaitable& operator=(RedissPoolInitializeAwaitable&&) noexcept = default;

        bool await_ready() { return m_inner.await_ready(); }
        template <typename Promise>
        bool await_suspend(std::coroutine_handle<Promise> handle)
        {
            return m_inner.await_suspend(handle);
        }
        Result await_resume() { return m_inner.await_resume(); }
        void markTimeout() { m_inner.markTimeout(); }

    private:
        struct Flow
        {
            explicit Flow(RedissConnectionPool& pool);

            void run(galay::kernel::SequenceOps<Result, 4>& ops);

            RedissConnectionPool* m_pool = nullptr;
        };

        using InnerAwaitable =
            galay::kernel::StateMachineAwaitable<typename galay::kernel::AwaitableBuilder<Result, 4, Flow>::MachineT>;

        galay::kernel::IOController m_controller{GHandle::invalid()};
        std::unique_ptr<Flow> m_flow;
        InnerAwaitable m_inner;
    };

    /**
     * @brief Rediss 连接池获取连接等待体
     * @details 用于协程中等待从 Rediss 连接池获取可用连接
     */
    class RedissPoolAcquireAwaitable : public galay::kernel::TimeoutSupport<RedissPoolAcquireAwaitable>
    {
    public:
        using Result = std::expected<std::shared_ptr<PooledRedissConnection>, RedisError>;

        explicit RedissPoolAcquireAwaitable(RedissConnectionPool& pool);
        RedissPoolAcquireAwaitable(const RedissPoolAcquireAwaitable&) = delete;
        RedissPoolAcquireAwaitable& operator=(const RedissPoolAcquireAwaitable&) = delete;
        RedissPoolAcquireAwaitable(RedissPoolAcquireAwaitable&&) noexcept = default;
        RedissPoolAcquireAwaitable& operator=(RedissPoolAcquireAwaitable&&) noexcept = default;

        bool await_ready() const noexcept { return false; }
        template <typename Promise>
        requires requires(const Promise& promise) {
            { promise.taskRefView() } -> std::same_as<const galay::kernel::TaskRef&>;
        }
        bool await_suspend(std::coroutine_handle<Promise> handle)
        {
            const auto action = prepareSuspend(galay::kernel::Waker(handle));
            if (action == SuspendAction::Wait) {
                return true;
            }
            if (action == SuspendAction::Connect && m_connect_awaitable.has_value()) {
                return m_connect_awaitable->await_suspend(handle);
            }
            return false;
        }
        Result await_resume();
        void markTimeout();

    private:
        enum class SuspendAction {
            Ready,
            Connect,
            Wait,
            Error,
        };

        enum class State {
            Invalid,
            Ready,
            Creating,
            Waiting,
            EnqueueFailed,
            TimedOut,
            Error,
        };

        SuspendAction prepareSuspend(galay::kernel::Waker waiter_waker);

        std::optional<detail::RedissConnectOperation> m_connect_awaitable;
        std::optional<RedisError> m_error;
        std::shared_ptr<PooledRedissConnection> m_connection;
        std::shared_ptr<detail::RedissPoolWaiter> m_waiter;
        RedissConnectionPool* m_pool = nullptr;
        std::chrono::steady_clock::time_point m_start_time;
        State m_state = State::Invalid;
        bool m_wait_counted = false;
    };
#endif

    /**
     * @brief Redis 连接池
     * @details 提供连接复用、自动扩缩容、健康检查等功能
     */
    class RedisConnectionPool
    {
    public:
        /**
         * @brief 构造函数
         * @param scheduler IO调度器
         * @param config 连接池配置
         */
        RedisConnectionPool(IOScheduler* scheduler, ConnectionPoolConfig config = ConnectionPoolConfig::defaultConfig());

        // 禁止拷贝
        RedisConnectionPool(const RedisConnectionPool&) = delete;
        RedisConnectionPool& operator=(const RedisConnectionPool&) = delete;

        // 禁止移动（内部状态需要稳定地址）
        RedisConnectionPool(RedisConnectionPool&&) = delete;
        RedisConnectionPool& operator=(RedisConnectionPool&&) = delete;

        /**
         * @brief 初始化连接池
         * @return 初始化等待体
         */
        PoolInitializeAwaitable initialize();

        /**
         * @brief 获取连接（协程安全）
         * @return 连接获取等待体
         */
        PoolAcquireAwaitable acquire();

        /**
         * @brief 归还连接
         * @param conn 要归还的连接
         */
        void release(std::shared_ptr<PooledConnection> conn);

        /**
         * @brief 手动触发健康检查
         */
        void triggerHealthCheck();

        /**
         * @brief 手动触发空闲连接清理
         */
        void triggerIdleCleanup();

        /**
         * @brief 预热连接池（创建到最小连接数）
         */
        void warmup();

        /**
         * @brief 清理所有不健康的连接
         */
        size_t cleanupUnhealthyConnections();

        /**
         * @brief 扩容连接池（创建指定数量的连接）
         * @param count 要创建的连接数
         * @return 实际创建的连接数
         */
        size_t expandPool(size_t count);

        /**
         * @brief 缩容连接池（移除空闲连接到目标数量）
         * @param target_size 目标连接数
         * @return 实际移除的连接数
         */
        size_t shrinkPool(size_t target_size);

        /**
         * @brief 关闭连接池（同步方法）
         */
        void shutdown();

        /**
         * @brief 获取连接池统计信息
         */
        struct PoolStats
        {
            size_t total_connections;      // 总连接数
            size_t available_connections;  // 可用连接数
            size_t active_connections;     // 活跃连接数
            size_t waiting_requests;       // 等待中的请求数
            uint64_t total_acquired;       // 总获取次数
            uint64_t total_released;       // 总归还次数
            uint64_t total_created;        // 总创建次数
            uint64_t total_destroyed;      // 总销毁次数
            uint64_t health_check_failures;// 健康检查失败次数
            uint64_t reconnect_attempts;   // 重连尝试次数
            uint64_t reconnect_successes;  // 重连成功次数
            uint64_t validation_failures;  // 验证失败次数

            // 性能监控指标
            double avg_acquire_time_ms;    // 平均获取连接时间（毫秒）
            double max_acquire_time_ms;    // 最大获取连接时间（毫秒）
            size_t peak_active_connections;// 峰值活跃连接数
            uint64_t total_acquire_time_ms;// 总获取时间（用于计算平均值）
        };

        PoolStats getStats() const;

        /**
         * @brief 获取配置
         */
        const ConnectionPoolConfig& getConfig() const { return m_config; }

        ~RedisConnectionPool();

    private:
        friend class PoolInitializeAwaitable;
        friend class PoolAcquireAwaitable;

        RedisVoidResult initializeSync(); ///< 同步初始化实现
        std::expected<std::shared_ptr<PooledConnection>, RedisError>
        acquireSync(std::chrono::steady_clock::time_point start_time); ///< 同步获取连接实现
        void recordAcquireStats(std::chrono::steady_clock::time_point start_time); ///< 记录获取连接统计

        /**
         * @brief 获取或创建连接（内部方法，同步）
         */
        std::expected<std::shared_ptr<PooledConnection>, RedisError> getConnectionSync();

        /**
         * @brief 检查连接健康状态（同步）
         */
        bool checkConnectionHealthSync(std::shared_ptr<PooledConnection> conn);

    private:
        IOScheduler* m_scheduler;                                            ///< IO 调度器
        ConnectionPoolConfig m_config;                                       ///< 连接池配置

        // 连接管理
        std::queue<std::shared_ptr<PooledConnection>> m_available_connections; ///< 可用连接队列
        std::queue<std::shared_ptr<detail::RedisPoolWaiter>> m_waiters;         ///< 等待连接的协程队列
        std::vector<std::shared_ptr<PooledConnection>> m_all_connections;      ///< 所有连接列表
        mutable std::mutex m_mutex;                                            ///< 互斥锁

        // 统计信息
        std::atomic<uint64_t> m_total_acquired{0};       ///< 总获取次数
        std::atomic<uint64_t> m_total_released{0};       ///< 总归还次数
        std::atomic<uint64_t> m_total_created{0};        ///< 总创建次数
        std::atomic<uint64_t> m_total_destroyed{0};      ///< 总销毁次数
        std::atomic<uint64_t> m_health_check_failures{0}; ///< 健康检查失败次数
        std::atomic<size_t> m_active_connections{0};     ///< 当前借出的连接数
        std::atomic<size_t> m_waiting_requests{0};       ///< 等待中的请求数
        std::atomic<uint64_t> m_reconnect_attempts{0};   ///< 重连尝试次数
        std::atomic<uint64_t> m_reconnect_successes{0};  ///< 重连成功次数
        std::atomic<uint64_t> m_validation_failures{0};  ///< 验证失败次数

        // 性能监控
        std::atomic<uint64_t> m_total_acquire_time_ms{0};  ///< 总获取时间（毫秒）
        std::atomic<double> m_max_acquire_time_ms{0.0};    ///< 最大获取时间（毫秒）
        std::atomic<size_t> m_peak_active_connections{0};  ///< 峰值活跃连接数
        std::atomic<bool> m_is_initialized{false};         ///< 是否已初始化
        std::atomic<bool> m_is_shutting_down{false};       ///< 是否正在关闭

    };

#ifdef GALAY_SSL_FEATURE_ENABLED
    /**
     * @brief Rediss（TLS）连接池
     * @details 与 RedisConnectionPool 功能一致，但使用 TLS 加密连接，
     *          适用于需要安全通信的场景
     */
    class RedissConnectionPool
    {
    public:
        using PoolStats = RedisConnectionPool::PoolStats; ///< 连接池统计信息类型别名

        /**
         * @brief 构造 Rediss 连接池
         * @param scheduler IO 调度器
         * @param config Rediss 连接池配置
         */
        RedissConnectionPool(IOScheduler* scheduler,
                             RedissConnectionPoolConfig config = RedissConnectionPoolConfig::defaultConfig());

        RedissConnectionPool(const RedissConnectionPool&) = delete; ///< 禁止拷贝
        RedissConnectionPool& operator=(const RedissConnectionPool&) = delete; ///< 禁止拷贝赋值
        RedissConnectionPool(RedissConnectionPool&&) = delete; ///< 禁止移动
        RedissConnectionPool& operator=(RedissConnectionPool&&) = delete; ///< 禁止移动赋值

        RedissPoolInitializeAwaitable initialize(); ///< 初始化连接池
        RedissPoolAcquireAwaitable acquire(); ///< 获取连接
        void release(std::shared_ptr<PooledRedissConnection> conn); ///< 归还连接
        void triggerHealthCheck(); ///< 手动触发健康检查
        void triggerIdleCleanup(); ///< 手动触发空闲连接清理
        void warmup(); ///< 预热连接池
        size_t cleanupUnhealthyConnections(); ///< 清理不健康的连接
        size_t expandPool(size_t count); ///< 扩容连接池
        size_t shrinkPool(size_t target_size); ///< 缩容连接池
        void shutdown(); ///< 关闭连接池
        PoolStats getStats() const; ///< 获取连接池统计信息
        const RedissConnectionPoolConfig& getConfig() const { return m_config; } ///< 获取配置

        ~RedissConnectionPool();

    private:
        friend class RedissPoolInitializeAwaitable;
        friend class RedissPoolAcquireAwaitable;

        RedisVoidResult initializeSync(); ///< 同步初始化实现
        std::expected<std::shared_ptr<PooledRedissConnection>, RedisError>
        acquireSync(std::chrono::steady_clock::time_point start_time); ///< 同步获取连接实现
        void recordAcquireStats(std::chrono::steady_clock::time_point start_time); ///< 记录获取连接统计
        std::expected<std::shared_ptr<PooledRedissConnection>, RedisError> getConnectionSync(); ///< 获取或创建连接
        bool checkConnectionHealthSync(std::shared_ptr<PooledRedissConnection> conn); ///< 检查连接健康状态

    private:
        IOScheduler* m_scheduler;                                                    ///< IO 调度器
        RedissConnectionPoolConfig m_config;                                         ///< 连接池配置
        std::queue<std::shared_ptr<PooledRedissConnection>> m_available_connections; ///< 可用连接队列
        std::queue<std::shared_ptr<detail::RedissPoolWaiter>> m_waiters;             ///< 等待连接的协程队列
        std::vector<std::shared_ptr<PooledRedissConnection>> m_all_connections;      ///< 所有连接列表
        mutable std::mutex m_mutex;                                                  ///< 互斥锁

        std::atomic<uint64_t> m_total_acquired{0};       ///< 总获取次数
        std::atomic<uint64_t> m_total_released{0};       ///< 总归还次数
        std::atomic<uint64_t> m_total_created{0};        ///< 总创建次数
        std::atomic<uint64_t> m_total_destroyed{0};      ///< 总销毁次数
        std::atomic<uint64_t> m_health_check_failures{0}; ///< 健康检查失败次数
        std::atomic<size_t> m_active_connections{0};     ///< 当前借出的连接数
        std::atomic<size_t> m_waiting_requests{0};       ///< 等待中的请求数
        std::atomic<uint64_t> m_reconnect_attempts{0};   ///< 重连尝试次数
        std::atomic<uint64_t> m_reconnect_successes{0};  ///< 重连成功次数
        std::atomic<uint64_t> m_validation_failures{0};  ///< 验证失败次数

        std::atomic<uint64_t> m_total_acquire_time_ms{0};  ///< 总获取时间（毫秒）
        std::atomic<double> m_max_acquire_time_ms{0.0};    ///< 最大获取时间（毫秒）
        std::atomic<size_t> m_peak_active_connections{0};  ///< 峰值活跃连接数
        std::atomic<bool> m_is_initialized{false};         ///< 是否已初始化
        std::atomic<bool> m_is_shutting_down{false};       ///< 是否正在关闭
    };
#endif

    /**
     * @brief RAII 风格的连接获取器
     * @details 自动归还连接到连接池
     */
    class ScopedConnection
    {
    public:
        ScopedConnection(RedisConnectionPool& pool, std::shared_ptr<PooledConnection> conn)
            : m_pool(&pool)
            , m_conn(std::move(conn))
        {
        }

        // 禁止拷贝
        ScopedConnection(const ScopedConnection&) = delete;
        ScopedConnection& operator=(const ScopedConnection&) = delete;

        // 允许移动
        ScopedConnection(ScopedConnection&& other) noexcept
            : m_pool(other.m_pool)
            , m_conn(std::move(other.m_conn))
        {
            other.m_pool = nullptr;
        }

        ScopedConnection& operator=(ScopedConnection&& other) noexcept
        {
            if (this != &other) {
                release();
                m_pool = other.m_pool;
                m_conn = std::move(other.m_conn);
                other.m_pool = nullptr;
            }
            return *this;
        }

        RedisClient<>* get() { return m_conn ? m_conn->get() : nullptr; }
        const RedisClient<>* get() const { return m_conn ? m_conn->get() : nullptr; }

        RedisClient<>* operator->() { return get(); }
        const RedisClient<>* operator->() const { return get(); }

        RedisClient<>& operator*() { return *get(); }
        const RedisClient<>& operator*() const { return *get(); }

        explicit operator bool() const { return m_conn != nullptr; }

        void release()
        {
            if (m_pool && m_conn) {
                m_pool->release(std::move(m_conn));
                m_conn = nullptr;
            }
        }

        ~ScopedConnection()
        {
            release();
        }

    private:
        RedisConnectionPool* m_pool;
        std::shared_ptr<PooledConnection> m_conn;
    };

#ifdef GALAY_SSL_FEATURE_ENABLED
    /**
     * @brief RAII 风格的 Rediss 连接获取器
     * @details 自动归还 Rediss 连接到连接池，析构时自动释放
     */
    class ScopedRedissConnection
    {
    public:
        /**
         * @brief 构造 ScopedRedissConnection
         * @param pool Rediss 连接池引用
         * @param conn 已获取的 Rediss 连接
         */
        ScopedRedissConnection(RedissConnectionPool& pool, std::shared_ptr<PooledRedissConnection> conn)
            : m_pool(&pool)
            , m_conn(std::move(conn))
        {
        }

        ScopedRedissConnection(const ScopedRedissConnection&) = delete; ///< 禁止拷贝
        ScopedRedissConnection& operator=(const ScopedRedissConnection&) = delete; ///< 禁止拷贝赋值

        /**
         * @brief 移动构造函数
         * @param other 另一个 ScopedRedissConnection
         */
        ScopedRedissConnection(ScopedRedissConnection&& other) noexcept
            : m_pool(other.m_pool)
            , m_conn(std::move(other.m_conn))
        {
            other.m_pool = nullptr;
        }

        /**
         * @brief 移动赋值运算符
         * @param other 另一个 ScopedRedissConnection
         * @return 当前对象引用
         */
        ScopedRedissConnection& operator=(ScopedRedissConnection&& other) noexcept
        {
            if (this != &other) {
                release();
                m_pool = other.m_pool;
                m_conn = std::move(other.m_conn);
                other.m_pool = nullptr;
            }
            return *this;
        }

        RedissClient* get() { return m_conn ? m_conn->get() : nullptr; }              ///< 获取原始客户端指针
        const RedissClient* get() const { return m_conn ? m_conn->get() : nullptr; }  ///< 获取原始客户端指针（const）

        RedissClient* operator->() { return get(); }              ///< 箭头操作符访问客户端
        const RedissClient* operator->() const { return get(); }  ///< 箭头操作符访问客户端（const）

        RedissClient& operator*() { return *get(); }              ///< 解引用操作符
        const RedissClient& operator*() const { return *get(); }  ///< 解引用操作符（const）

        explicit operator bool() const { return m_conn != nullptr; } ///< 检查是否持有有效连接

        /**
         * @brief 手动释放连接，归还到连接池
         */
        void release()
        {
            if (m_pool && m_conn) {
                m_pool->release(std::move(m_conn));
                m_conn = nullptr;
            }
        }

        /**
         * @brief 析构函数，自动归还连接
         */
        ~ScopedRedissConnection()
        {
            release();
        }

    private:
        RedissConnectionPool* m_pool;                                    ///< 连接池指针
        std::shared_ptr<PooledRedissConnection> m_conn;                  ///< 持有的连接
    };
#endif

} // namespace galay::redis

#endif // GALAY_REDIS_CONNECTION_POOL_H
