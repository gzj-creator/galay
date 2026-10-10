/**
 * @file conn_pool.h
 * @brief Redis 连接池管理
 * @author galay-redis
 * @version 1.0.0
 *
 * @details 提供 Redis/Rediss 连接池的配置、连接包装和连接池实现；协程等待体
 *          分离在 details/pool_awaitable.h/.inl。
 */

#ifndef GALAY_REDIS_CONNECTION_POOL_H
#define GALAY_REDIS_CONNECTION_POOL_H

#include "client.h"
#include "conn_pool_waiter_state.h"
#include "../../galay-kernel/core/awaitable.h"
#include "../../galay-kernel/core/io_scheduler.hpp"
#include "../../galay-kernel/core/waker.h"
#include "../../galay-utils/common/defn.hpp"
#include <galay/thirdparty/concurrentqueue/moodycamel/concurrentqueue.h>
#include <array>
#include <memory>
#include <vector>
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
        static ConnectionPoolConfig default_config()
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
        static RedissConnectionPoolConfig default_config()
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

    private:
        PooledConnection(const PooledConnection&) = delete; ///< 禁止拷贝连接包装器
        /**
         * @brief 禁止拷贝赋值
         * @return 该操作已禁用，不可调用
         */
        PooledConnection& operator=(const PooledConnection&) = delete;
    public:
        PooledConnection(PooledConnection&&) noexcept = default; ///< 允许移动所有权
        /**
         * @brief 允许移动赋值
         * @return 当前对象引用
         */
        PooledConnection& operator=(PooledConnection&&) noexcept = default;

        /**
         * @brief 获取原始客户端指针
         * @return RedisClient<>* 指针
         */
        RedisClient<>* get() { return m_client.get(); }
        /**
         * @brief 获取原始客户端指针（const）
         * @return const RedisClient<>* 指针
         */
        const RedisClient<>* get() const { return m_client.get(); }

        /**
         * @brief 箭头操作符访问客户端
         * @return RedisClient<>* 指针
         */
        RedisClient<>* operator->() { return m_client.get(); }
        /**
         * @brief 箭头操作符访问客户端（const）
         * @return const RedisClient<>* 指针
         */
        const RedisClient<>* operator->() const { return m_client.get(); }

        /**
         * @brief 解引用操作符
         * @return RedisClient<>& 引用
         */
        RedisClient<>& operator*() { return *m_client; }
        /**
         * @brief 解引用操作符（const）
         * @return const RedisClient<>& 引用
         */
        const RedisClient<>& operator*() const { return *m_client; }

        /**
         * @brief 更新最后使用时间为当前时刻
         * @return 无返回值
         */
        void update_last_used()
        {
            m_last_used = std::chrono::steady_clock::now();
        }

        /**
         * @brief 获取连接空闲时间
         * @return 自上次使用以来经过的毫秒数
         */
        std::chrono::milliseconds get_idle_time() const
        {
            auto now = std::chrono::steady_clock::now();
            return std::chrono::duration_cast<std::chrono::milliseconds>(now - m_last_used);
        }

        /**
         * @brief 获取健康状态
         * @return 满足所检查条件时返回 true，否则返回 false
         */
        bool is_healthy() const { return m_is_healthy; }
        /**
         * @brief 设置健康状态
         * @param healthy 是否健康
         * @return 无返回值
         */
        void set_healthy(bool healthy) { m_is_healthy = healthy; }
        /**
         * @brief 检查连接是否已关闭
         * @return 满足所检查条件时返回 true，否则返回 false
         */
        bool is_closed() const { return m_client->is_closed(); }

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

    private:
        PooledRedissConnection(const PooledRedissConnection&) = delete; ///< 禁止拷贝连接包装器
        /**
         * @brief 禁止拷贝赋值
         * @return 该操作已禁用，不可调用
         */
        PooledRedissConnection& operator=(const PooledRedissConnection&) = delete;
    public:
        PooledRedissConnection(PooledRedissConnection&&) noexcept = default; ///< 允许移动所有权
        /**
         * @brief 允许移动赋值
         * @return 当前对象引用
         */
        PooledRedissConnection& operator=(PooledRedissConnection&&) noexcept = default;

        /**
         * @brief 获取原始客户端指针
         * @return RedissClient* 指针
         */
        RedissClient* get() { return m_client.get(); }
        /**
         * @brief 获取原始客户端指针（const）
         * @return const RedissClient* 指针
         */
        const RedissClient* get() const { return m_client.get(); }

        /**
         * @brief 箭头操作符访问客户端
         * @return RedissClient* 指针
         */
        RedissClient* operator->() { return m_client.get(); }
        /**
         * @brief 箭头操作符访问客户端（const）
         * @return const RedissClient* 指针
         */
        const RedissClient* operator->() const { return m_client.get(); }

        /**
         * @brief 解引用操作符
         * @return RedissClient& 引用
         */
        RedissClient& operator*() { return *m_client; }
        /**
         * @brief 解引用操作符（const）
         * @return const RedissClient& 引用
         */
        const RedissClient& operator*() const { return *m_client; }

        /**
         * @brief 更新最后使用时间为当前时刻
         * @return 无返回值
         */
        void update_last_used()
        {
            m_last_used = std::chrono::steady_clock::now();
        }

        /**
         * @brief 获取连接空闲时间
         * @return 自上次使用以来经过的毫秒数
         */
        std::chrono::milliseconds get_idle_time() const
        {
            auto now = std::chrono::steady_clock::now();
            return std::chrono::duration_cast<std::chrono::milliseconds>(now - m_last_used);
        }

        /**
         * @brief 获取健康状态
         * @return 满足所检查条件时返回 true，否则返回 false
         */
        bool is_healthy() const { return m_is_healthy; }
        /**
         * @brief 设置健康状态
         * @param healthy 是否健康
         * @return 无返回值
         */
        void set_healthy(bool healthy) { m_is_healthy = healthy; }
        /**
         * @brief 检查连接是否已关闭
         * @return 满足所检查条件时返回 true，否则返回 false
         */
        bool is_closed() const { return m_client->is_closed(); }

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
    class PoolInitializeAwaitable;
    class PoolAcquireAwaitable;
#ifdef GALAY_SSL_FEATURE_ENABLED
    class RedissConnectionPool;
    class RedissPoolInitializeAwaitable;
    class RedissPoolAcquireAwaitable;
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
        RedisConnectionPool(IOScheduler* scheduler, ConnectionPoolConfig config = ConnectionPoolConfig::default_config());

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
         * @return 无返回值
         */
        void release(std::shared_ptr<PooledConnection> conn);

        /**
         * @brief 手动触发健康检查
         * @return 无返回值
         */
        void trigger_health_check();

        /**
         * @brief 手动触发空闲连接清理
         * @return 无返回值
         */
        void trigger_idle_cleanup();

        /**
         * @brief 预热连接池（创建到最小连接数）
         * @return 无返回值
         */
        void warmup();

        /**
         * @brief 清理所有不健康的连接
         * @return 清理的不健康连接数量
         */
        size_t cleanup_unhealthy_connections();

        /**
         * @brief 扩容连接池（创建指定数量的连接）
         * @param count 要创建的连接数
         * @return 实际创建的连接数
         */
        size_t expand_pool(size_t count);

        /**
         * @brief 缩容连接池（移除空闲连接到目标数量）
         * @param target_size 目标连接数
         * @return 实际移除的连接数
         */
        size_t shrink_pool(size_t target_size);

        /**
         * @brief 关闭连接池（同步方法）
         * @return 无返回值
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

        PoolStats get_stats() const;

        /**
         * @brief 获取配置
         * @return const ConnectionPoolConfig& 只读引用
         */
        const ConnectionPoolConfig& get_config() const { return m_config; }

        ~RedisConnectionPool();

    private:
        friend class PoolInitializeAwaitable;
        friend class PoolAcquireAwaitable;

        /**
         * @brief 同步初始化实现
         * @return RedisVoidResult 操作结果
         */
        RedisVoidResult initialize_sync();
        /**
         * @brief 同步获取连接实现
         * @param start_time 开始时刻
         * @return 成功时返回 std::shared_ptr<PooledConnection>，失败时返回 RedisError 错误
         */
        std::expected<std::shared_ptr<PooledConnection>, RedisError>
        acquire_sync(std::chrono::steady_clock::time_point start_time);
        /**
         * @brief 记录获取连接统计
         * @param start_time 开始时刻
         * @return 无返回值
         */
        void record_acquire_stats(std::chrono::steady_clock::time_point start_time);
        /**
         * @brief 从分片空闲队列非阻塞获取健康连接
         * @return std::shared_ptr<PooledConnection> 操作结果
         */
        std::shared_ptr<PooledConnection> try_acquire_available();
        /**
         * @brief 预留容量并创建待连接槽位
         * @return std::shared_ptr<PooledConnection> 操作结果
         */
        std::shared_ptr<PooledConnection> create_connection_slot();
        /**
         * @brief 销毁已计入容量的连接槽位
         * @param conn 连接对象
         * @return 无返回值
         */
        void destroy_connection_slot(std::shared_ptr<PooledConnection>& conn);
        /**
         * @brief 将健康连接放回分片空闲队列
         * @param conn 连接对象
         * @return 操作成功时返回 true，否则返回 false
         */
        bool return_to_available(std::shared_ptr<PooledConnection> conn);
        /**
         * @brief 注册等待连接的协程
         * @param waiter 等待节点
         * @return 操作成功时返回 true，否则返回 false
         */
        bool enqueue_waiter(std::shared_ptr<detail::RedisPoolWaiter> waiter);
        /**
         * @brief 尝试把连接转交给等待者
         * @param conn 连接对象
         * @param waiter_to_wake 要唤醒的等待节点
         * @return 操作成功时返回 true，否则返回 false
         */
        bool complete_one_waiter(std::shared_ptr<PooledConnection> conn,
                               std::shared_ptr<detail::RedisPoolWaiter>& waiter_to_wake);
        /**
         * @brief 从空闲队列唤醒一个等待者，避免 enqueue/release 竞态
         * @return 操作成功时返回 true，否则返回 false
         */
        bool wake_one_waiter_from_available();
        /**
         * @brief 清空空闲分片
         * @param drained 已排空条目数量
         * @return size_t 操作结果
         */
        size_t drain_available_connections(std::vector<std::shared_ptr<PooledConnection>>* drained = nullptr);
        /**
         * @brief 当前线程对应的空闲分片
         * @return size_t 操作结果
         */
        size_t idle_shard_index() const noexcept;

        /**
         * @brief 获取或创建连接（内部方法，同步）
         * @return 成功时返回 std::shared_ptr<PooledConnection>，失败时返回 RedisError 错误
         */
        std::expected<std::shared_ptr<PooledConnection>, RedisError> get_connection_sync();

        /**
         * @brief 检查连接健康状态（同步）
         * @param conn 连接对象
         * @return 连接健康时返回 true，否则返回 false
         */
        bool check_connection_health_sync(std::shared_ptr<PooledConnection> conn);

    private:
        static constexpr size_t kIdleShardCount = 16;
        static_assert((kIdleShardCount & (kIdleShardCount - 1)) == 0,
                      "Redis pool idle shard count must be a power of two");

        struct alignas(::galay::utils::kCacheLineSize) IdleShard
        {
            moodycamel::ConcurrentQueue<std::shared_ptr<PooledConnection>> available; ///< 分片空闲连接队列
        };

        IOScheduler* m_scheduler;                                            ///< IO 调度器
        ConnectionPoolConfig m_config;                                       ///< 连接池配置

        // 连接管理：空闲连接按线程分片，waiter 使用线程安全队列，协程路径不阻塞 OS 线程。
        std::array<IdleShard, kIdleShardCount> m_available_shards;             ///< 分片空闲连接队列
        moodycamel::ConcurrentQueue<std::shared_ptr<detail::RedisPoolWaiter>> m_waiters; ///< 等待连接的协程队列

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
        std::atomic<size_t> m_live_connections{0};         ///< 已创建或正在连接的连接槽位数
        std::atomic<size_t> m_idle_connections{0};         ///< 当前空闲队列中的连接数
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
                             RedissConnectionPoolConfig config = RedissConnectionPoolConfig::default_config());

        RedissConnectionPool(const RedissConnectionPool&) = delete; ///< 禁止拷贝
        /**
         * @brief 禁止拷贝赋值
         * @return 该操作已禁用，不可调用
         */
        RedissConnectionPool& operator=(const RedissConnectionPool&) = delete;
        RedissConnectionPool(RedissConnectionPool&&) = delete; ///< 禁止移动
        /**
         * @brief 禁止移动赋值
         * @return 该操作已禁用，不可调用
         */
        RedissConnectionPool& operator=(RedissConnectionPool&&) = delete;

        /**
         * @brief 初始化连接池
         * @return RedissPoolInitializeAwaitable 等待体，通过 co_await 执行并取得操作结果
         */
        RedissPoolInitializeAwaitable initialize();
        /**
         * @brief 获取连接
         * @return RedissPoolAcquireAwaitable 等待体，通过 co_await 执行并取得操作结果
         */
        RedissPoolAcquireAwaitable acquire();
        /**
         * @brief 归还连接
         * @param conn 连接对象
         * @return 无返回值
         */
        void release(std::shared_ptr<PooledRedissConnection> conn);
        /**
         * @brief 手动触发健康检查
         * @return 无返回值
         */
        void trigger_health_check();
        /**
         * @brief 手动触发空闲连接清理
         * @return 无返回值
         */
        void trigger_idle_cleanup();
        /**
         * @brief 预热连接池
         * @return 无返回值
         */
        void warmup();
        /**
         * @brief 清理不健康的连接
         * @return size_t 操作结果
         */
        size_t cleanup_unhealthy_connections();
        /**
         * @brief 扩容连接池
         * @param count 元素数量
         * @return size_t 操作结果
         */
        size_t expand_pool(size_t count);
        /**
         * @brief 缩容连接池
         * @param target_size 目标大小
         * @return size_t 操作结果
         */
        size_t shrink_pool(size_t target_size);
        /**
         * @brief 关闭连接池
         * @return 无返回值
         */
        void shutdown();
        /**
         * @brief 获取连接池统计信息
         * @return PoolStats 操作结果
         */
        PoolStats get_stats() const;
        /**
         * @brief 获取配置
         * @return const RedissConnectionPoolConfig& 引用
         */
        const RedissConnectionPoolConfig& get_config() const { return m_config; }

        ~RedissConnectionPool();

    private:
        friend class RedissPoolInitializeAwaitable;
        friend class RedissPoolAcquireAwaitable;

        /**
         * @brief 同步初始化实现
         * @return RedisVoidResult 操作结果
         */
        RedisVoidResult initialize_sync();
        /**
         * @brief 同步获取连接实现
         * @param start_time 开始时刻
         * @return 成功时返回 std::shared_ptr<PooledRedissConnection>，失败时返回 RedisError 错误
         */
        std::expected<std::shared_ptr<PooledRedissConnection>, RedisError>
        acquire_sync(std::chrono::steady_clock::time_point start_time);
        /**
         * @brief 记录获取连接统计
         * @param start_time 开始时刻
         * @return 无返回值
         */
        void record_acquire_stats(std::chrono::steady_clock::time_point start_time);
        /**
         * @brief 从分片空闲队列非阻塞获取健康连接
         * @return std::shared_ptr<PooledRedissConnection> 操作结果
         */
        std::shared_ptr<PooledRedissConnection> try_acquire_available();
        /**
         * @brief 预留容量并创建待连接槽位
         * @return std::shared_ptr<PooledRedissConnection> 操作结果
         */
        std::shared_ptr<PooledRedissConnection> create_connection_slot();
        /**
         * @brief 销毁已计入容量的连接槽位
         * @param conn 连接对象
         * @return 无返回值
         */
        void destroy_connection_slot(std::shared_ptr<PooledRedissConnection>& conn);
        /**
         * @brief 将健康连接放回分片空闲队列
         * @param conn 连接对象
         * @return 操作成功时返回 true，否则返回 false
         */
        bool return_to_available(std::shared_ptr<PooledRedissConnection> conn);
        /**
         * @brief 注册等待连接的协程
         * @param waiter 等待节点
         * @return 操作成功时返回 true，否则返回 false
         */
        bool enqueue_waiter(std::shared_ptr<detail::RedissPoolWaiter> waiter);
        /**
         * @brief 尝试把连接转交给等待者
         * @param conn 连接对象
         * @param waiter_to_wake 要唤醒的等待节点
         * @return 操作成功时返回 true，否则返回 false
         */
        bool complete_one_waiter(std::shared_ptr<PooledRedissConnection> conn,
                               std::shared_ptr<detail::RedissPoolWaiter>& waiter_to_wake);
        /**
         * @brief 从空闲队列唤醒一个等待者，避免 enqueue/release 竞态
         * @return 操作成功时返回 true，否则返回 false
         */
        bool wake_one_waiter_from_available();
        /**
         * @brief 清空空闲分片
         * @param drained 已排空条目数量
         * @return size_t 操作结果
         */
        size_t drain_available_connections(std::vector<std::shared_ptr<PooledRedissConnection>>* drained = nullptr);
        /**
         * @brief 当前线程对应的空闲分片
         * @return size_t 操作结果
         */
        size_t idle_shard_index() const noexcept;
        /**
         * @brief 获取或创建连接
         * @return 成功时返回 std::shared_ptr<PooledRedissConnection>，失败时返回 RedisError 错误
         */
        std::expected<std::shared_ptr<PooledRedissConnection>, RedisError> get_connection_sync();
        /**
         * @brief 检查连接健康状态
         * @param conn 连接对象
         * @return 操作成功时返回 true，否则返回 false
         */
        bool check_connection_health_sync(std::shared_ptr<PooledRedissConnection> conn);

    private:
        static constexpr size_t kIdleShardCount = 16;
        static_assert((kIdleShardCount & (kIdleShardCount - 1)) == 0,
                      "Rediss pool idle shard count must be a power of two");

        struct alignas(::galay::utils::kCacheLineSize) IdleShard
        {
            moodycamel::ConcurrentQueue<std::shared_ptr<PooledRedissConnection>> available; ///< 分片空闲连接队列
        };

        IOScheduler* m_scheduler;                                                    ///< IO 调度器
        RedissConnectionPoolConfig m_config;                                         ///< 连接池配置
        std::array<IdleShard, kIdleShardCount> m_available_shards;                   ///< 分片空闲连接队列
        moodycamel::ConcurrentQueue<std::shared_ptr<detail::RedissPoolWaiter>> m_waiters; ///< 等待连接的协程队列

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
        std::atomic<size_t> m_live_connections{0};         ///< 已创建或正在连接的连接槽位数
        std::atomic<size_t> m_idle_connections{0};         ///< 当前空闲队列中的连接数
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
        /**
         * @brief 禁止拷贝赋值
         * @return 该操作已禁用，不可调用
         */
        ScopedRedissConnection& operator=(const ScopedRedissConnection&) = delete;

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

        /**
         * @brief 获取原始客户端指针
         * @return RedissClient* 指针
         */
        RedissClient* get() { return m_conn ? m_conn->get() : nullptr; }
        /**
         * @brief 获取原始客户端指针（const）
         * @return const RedissClient* 指针
         */
        const RedissClient* get() const { return m_conn ? m_conn->get() : nullptr; }

        /**
         * @brief 箭头操作符访问客户端
         * @return RedissClient* 指针
         */
        RedissClient* operator->() { return get(); }
        /**
         * @brief 箭头操作符访问客户端（const）
         * @return const RedissClient* 指针
         */
        const RedissClient* operator->() const { return get(); }

        /**
         * @brief 解引用操作符
         * @return RedissClient& 引用
         */
        RedissClient& operator*() { return *get(); }
        /**
         * @brief 解引用操作符（const）
         * @return const RedissClient& 引用
         */
        const RedissClient& operator*() const { return *get(); }

        /**
         * @brief 检查是否持有有效连接
         * @return 持有有效资源时返回 true，否则返回 false
         */
        explicit operator bool() const { return m_conn != nullptr; }

        /**
         * @brief 手动释放连接，归还到连接池
         * @return 无返回值
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

#include "../details/pool_awaitable.h"

#endif // GALAY_REDIS_CONNECTION_POOL_H
