/**
 * @file topology_client.h
 * @brief Redis 主从和集群拓扑客户端
 * @author galay-redis
 * @version 1.0.0
 *
 * @details 提供 Redis 主从复制（Master-Slave）和集群（Cluster）模式的高级客户端，
 *          支持读写分离、Sentinel 自动发现、集群槽位路由和自动重试等功能。
 *          同时提供 TLS 版本（Rediss）的实现。
 */

#ifndef GALAY_REDIS_TOPOLOGY_CLIENT_H
#define GALAY_REDIS_TOPOLOGY_CLIENT_H

#include <cstdint>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <array>
#include <utility>
#include "../base/redis_config.h"
#include "redis_client.h"

namespace galay::redis
{
    using RedisCommandResult = std::expected<std::vector<RedisValue>, RedisError>; ///< 命令执行结果类型

    /**
     * @brief Redis 拓扑读路由策略
     * @details 该策略作为拓扑客户端的公开配置面保存，当前命令执行仍以 execute/batch
     *          调用中的 prefer_read 或 routing_key 参数作为具体路由依据。
     */
    enum class RedisReadPolicy
    {
        MasterOnly,       ///< 读请求只使用主节点
        PreferReplica,    ///< 优先使用副本，副本不可用时回退主节点
        ReplicaOnly,      ///< 读请求只使用副本
        RoundRobinReplica ///< 在副本之间轮询
    };

    /**
     * @brief Redis 拓扑访问模式
     */
    enum class RedisAccessMode
    {
        ReadWrite, ///< 可读写
        ReadOnly   ///< 只读
    };

    /**
     * @brief Redis 拓扑自动重试配置
     * @details max_attempts 表示一次拓扑命令最多尝试次数，0 会在构建客户端时按 1 处理。
     */
    struct RedisTopologyRetryConfig
    {
        size_t max_attempts = 2; ///< 最大尝试次数
    };

    /**
     * @brief Redis 拓扑刷新配置
     * @details refresh_interval 表示集群槽位等拓扑信息的自动刷新间隔，非正值会保留客户端默认值。
     */
    struct RedisTopologyRefreshConfig
    {
        std::chrono::milliseconds refresh_interval{5000}; ///< 自动刷新间隔
    };

    /**
     * @brief Redis 拓扑客户端统计信息
     */
    struct RedisTopologyStats
    {
        size_t master_count = 0;            ///< 已配置主节点数量
        size_t replica_count = 0;           ///< 已配置副本数量
        size_t sentinel_count = 0;          ///< 已配置 Sentinel 数量
        size_t cluster_node_count = 0;      ///< 已配置集群节点数量
        size_t connected_node_count = 0;    ///< 已标记为已连接的节点数量
        RedisTopologyRetryConfig retry_config;       ///< 重试配置
        RedisTopologyRefreshConfig refresh_config;   ///< 刷新配置
        RedisReadPolicy read_policy = RedisReadPolicy::PreferReplica; ///< 读路由策略
        RedisAccessMode access_mode = RedisAccessMode::ReadWrite;     ///< 访问模式
        bool master_connected = false;      ///< 主节点是否已标记为已连接
        bool slot_cache_ready = false;      ///< 集群槽位缓存是否已就绪
    };

    /**
     * @brief Redis 节点地址
     * @details 包含 Redis 节点的连接信息和认证参数
     */
    struct RedisNodeAddress
    {
        std::string host = "127.0.0.1";    ///< 节点主机地址
        std::string username;               ///< 认证用户名
        std::string password;               ///< 认证密码
        int32_t port = 6379;                ///< 节点端口
        int32_t db_index = 0;               ///< 数据库索引
        int version = 2;                    ///< RESP 协议版本
    };

    class RedisMasterSlaveClient;

    /**
     * @brief Redis 主从客户端构建器
     * @details 使用建造者模式逐步配置并构建 RedisMasterSlaveClient 实例
     */
    class RedisMasterSlaveClientBuilder
    {
    public:
        RedisMasterSlaveClientBuilder& scheduler(IOScheduler* scheduler) ///< 设置 IO 调度器
        {
            m_scheduler = scheduler;
            return *this;
        }

        RedisMasterSlaveClientBuilder& config(AsyncRedisConfig config) ///< 设置异步配置
        {
            m_config = std::move(config);
            return *this;
        }

        RedisMasterSlaveClientBuilder& sendTimeout(std::chrono::milliseconds timeout) ///< 设置发送超时
        {
            m_config.send_timeout = timeout;
            return *this;
        }

        RedisMasterSlaveClientBuilder& recvTimeout(std::chrono::milliseconds timeout) ///< 设置接收超时
        {
            m_config.recv_timeout = timeout;
            return *this;
        }

        RedisMasterSlaveClientBuilder& bufferSize(size_t size) ///< 设置缓冲区大小
        {
            m_config.buffer_size = size;
            return *this;
        }

        RedisMasterSlaveClientBuilder& retryConfig(RedisTopologyRetryConfig config) ///< 设置拓扑重试配置
        {
            m_retry_config = config;
            return *this;
        }

        RedisMasterSlaveClientBuilder& refreshConfig(RedisTopologyRefreshConfig config) ///< 设置拓扑刷新配置
        {
            m_refresh_config = config;
            return *this;
        }

        RedisMasterSlaveClientBuilder& readPolicy(RedisReadPolicy policy) ///< 设置读路由策略
        {
            m_read_policy = policy;
            return *this;
        }

        /**
         * @brief 构建主从客户端
         * @return RedisMasterSlaveClient 实例
         */
        RedisMasterSlaveClient build() const;

        /**
         * @brief 获取当前配置
         * @return 异步 Redis 配置
         */
        AsyncRedisConfig buildConfig() const
        {
            return m_config;
        }

    private:
        IOScheduler* m_scheduler = nullptr;                         ///< IO 调度器
        AsyncRedisConfig m_config = AsyncRedisConfig::noTimeout();  ///< 异步配置
        RedisTopologyRetryConfig m_retry_config;                    ///< 拓扑重试配置
        RedisTopologyRefreshConfig m_refresh_config;                ///< 拓扑刷新配置
        RedisReadPolicy m_read_policy = RedisReadPolicy::PreferReplica; ///< 读路由策略
    };

    /**
     * @brief Redis 主从客户端
     * @details 支持主从读写分离、自动故障转移（通过 Sentinel）和读写负载均衡
     */
    class RedisMasterSlaveClient
    {
    public:
        /**
         * @brief 构造主从客户端
         * @param scheduler IO 调度器
         * @param config 异步 Redis 配置
         */
        explicit RedisMasterSlaveClient(IOScheduler* scheduler,
                                        AsyncRedisConfig config = AsyncRedisConfig::noTimeout());

        /**
         * @brief 构造主从客户端并保存拓扑配置
         * @param scheduler IO 调度器
         * @param config 异步 Redis 配置
         * @param retry_config 拓扑重试配置
         * @param refresh_config 拓扑刷新配置
         * @param read_policy 读路由策略
         */
        explicit RedisMasterSlaveClient(IOScheduler* scheduler,
                                        AsyncRedisConfig config,
                                        RedisTopologyRetryConfig retry_config,
                                        RedisTopologyRefreshConfig refresh_config,
                                        RedisReadPolicy read_policy);

        /**
         * @brief 连接到主节点
         * @param master 主节点地址
         * @return 连接操作等待体
         */
        RedisConnectOperation connectMaster(const RedisNodeAddress& master);

        /**
         * @brief 添加并连接从节点
         * @param replica 从节点地址
         * @return 连接操作等待体
         */
        RedisConnectOperation addReplica(const RedisNodeAddress& replica);

        /**
         * @brief 执行 Redis 命令（支持读写分离和自动重试）
         * @param cmd 命令名称
         * @param args 命令参数
         * @param prefer_read 是否优先在读副本执行
         * @param auto_retry 是否自动重试
         * @return 命令执行结果协程任务
         */
        Task<RedisCommandResult> execute(const std::string& cmd,
                                         const std::vector<std::string>& args,
                                         bool prefer_read = false,
                                         bool auto_retry = true);

        /**
         * @brief 批量执行命令（Pipeline）
         * @param commands 命令视图数组
         * @param prefer_read 是否优先在读副本执行
         * @return 命令交换操作等待体
         */
        RedisExchangeOperation batch(std::span<const RedisCommandView> commands,
                                     bool prefer_read = false);

        /**
         * @brief 添加 Sentinel 节点
         * @param sentinel Sentinel 节点地址
         * @return 连接操作等待体
         */
        RedisConnectOperation addSentinel(const RedisNodeAddress& sentinel);

        /**
         * @brief 设置 Sentinel 监控的主节点名称
         * @param master_name 主节点名称
         */
        void setSentinelMasterName(std::string master_name);

        /**
         * @brief 设置自动重试次数
         * @param attempts 最大重试次数
         */
        void setAutoRetryAttempts(size_t attempts) noexcept;

        /**
         * @brief 从 Sentinel 刷新主节点信息
         * @return 命令执行结果协程任务
         */
        Task<RedisCommandResult> refreshFromSentinel();

        RedisClient<>& master(); ///< 获取主节点客户端引用
        /**
         * @brief 获取指定索引的从节点客户端
         * @param index 从节点索引
         * @return 从节点客户端引用（若索引越界返回空）
         */
        std::optional<std::reference_wrapper<RedisClient<>>> replica(size_t index);
        size_t replicaCount() const noexcept; ///< 获取从节点数量

        /**
         * @brief 获取拓扑统计信息
         * @return 当前已配置节点、连接标记和拓扑配置快照
         */
        RedisTopologyStats getStats() const noexcept;

    private:
        /**
         * @brief 节点句柄，管理节点地址和客户端连接
         */
        struct NodeHandle
        {
            RedisNodeAddress address;                          ///< 节点地址
            std::unique_ptr<RedisClient<>> client;               ///< 客户端实例
            bool connected = false;                            ///< 连接状态
        };

        Task<RedisCommandResult> runAutoTask(bool prefer_read,
                                             std::string cmd,
                                             std::vector<std::string> args,
                                             size_t max_attempts); ///< 自动重试执行协程
        Task<RedisCommandResult> refreshSentinelTask(); ///< Sentinel 刷新协程

        bool isRetryableConnectionError(const RedisError& error) const noexcept; ///< 判断是否为可重试的连接错误
        RedisClient<>* chooseReadClient(); ///< 选择读客户端（轮询从节点）
        RedisClient<>* ensureMaster(); ///< 确保主节点可用
        RedisClient<>* chooseAvailableSentinel(); ///< 选择可用的 Sentinel 节点
        bool parseMasterAddressReply(const std::vector<RedisValue>& values, RedisNodeAddress* out_addr) const; ///< 解析主节点地址回复
        bool parseReplicaListReply(const std::vector<RedisValue>& values, std::vector<RedisNodeAddress>* replicas) const; ///< 解析从节点列表回复

        IOScheduler* m_scheduler;                                     ///< IO 调度器
        AsyncRedisConfig m_config;                                    ///< 异步配置
        std::unique_ptr<RedisClient<>> m_master;                        ///< 主节点客户端
        RedisNodeAddress m_master_address;                            ///< 主节点地址
        std::vector<std::unique_ptr<RedisClient<>>> m_replicas;         ///< 从节点客户端列表
        std::vector<RedisNodeAddress> m_replica_addresses;            ///< 从节点地址列表
        std::vector<bool> m_replica_connected;                        ///< 从节点连接状态
        std::vector<NodeHandle> m_sentinels;                          ///< Sentinel 节点列表
        std::string m_sentinel_master_name = "mymaster";              ///< Sentinel 监控的主节点名称
        size_t m_read_cursor = 0;                                     ///< 读请求轮询游标
        size_t m_auto_retry_attempts = 2;                             ///< 自动重试次数
        RedisTopologyRetryConfig m_retry_config;                      ///< 拓扑重试配置
        RedisTopologyRefreshConfig m_refresh_config;                  ///< 拓扑刷新配置
        RedisReadPolicy m_read_policy = RedisReadPolicy::PreferReplica; ///< 读路由策略
        bool m_master_connected = false;                              ///< 主节点连接状态
    };

    /**
     * @brief Redis 集群节点地址
     * @details 扩展 RedisNodeAddress，增加槽位范围信息
     */
    struct RedisClusterNodeAddress : RedisNodeAddress
    {
        uint16_t slot_start = 0;      ///< 槽位起始值
        uint16_t slot_end = 16383;    ///< 槽位结束值
    };

    class RedisClusterClient;

    /**
     * @brief Redis 集群客户端构建器
     * @details 使用建造者模式逐步配置并构建 RedisClusterClient 实例
     */
    class RedisClusterClientBuilder
    {
    public:
        RedisClusterClientBuilder& scheduler(IOScheduler* scheduler) ///< 设置 IO 调度器
        {
            m_scheduler = scheduler;
            return *this;
        }

        RedisClusterClientBuilder& config(AsyncRedisConfig config) ///< 设置异步配置
        {
            m_config = std::move(config);
            return *this;
        }

        RedisClusterClientBuilder& sendTimeout(std::chrono::milliseconds timeout) ///< 设置发送超时
        {
            m_config.send_timeout = timeout;
            return *this;
        }

        RedisClusterClientBuilder& recvTimeout(std::chrono::milliseconds timeout) ///< 设置接收超时
        {
            m_config.recv_timeout = timeout;
            return *this;
        }

        RedisClusterClientBuilder& bufferSize(size_t size) ///< 设置缓冲区大小
        {
            m_config.buffer_size = size;
            return *this;
        }

        RedisClusterClientBuilder& retryConfig(RedisTopologyRetryConfig config) ///< 设置拓扑重试配置
        {
            m_retry_config = config;
            return *this;
        }

        RedisClusterClientBuilder& refreshConfig(RedisTopologyRefreshConfig config) ///< 设置拓扑刷新配置
        {
            m_refresh_config = config;
            return *this;
        }

        RedisClusterClientBuilder& readPolicy(RedisReadPolicy policy) ///< 设置读路由策略
        {
            m_read_policy = policy;
            return *this;
        }

        /**
         * @brief 构建集群客户端
         * @return RedisClusterClient 实例
         */
        RedisClusterClient build() const;

        /**
         * @brief 获取当前配置
         * @return 异步 Redis 配置
         */
        AsyncRedisConfig buildConfig() const
        {
            return m_config;
        }

    private:
        IOScheduler* m_scheduler = nullptr;                         ///< IO 调度器
        AsyncRedisConfig m_config = AsyncRedisConfig::noTimeout();  ///< 异步配置
        RedisTopologyRetryConfig m_retry_config;                    ///< 拓扑重试配置
        RedisTopologyRefreshConfig m_refresh_config;                ///< 拓扑刷新配置
        RedisReadPolicy m_read_policy = RedisReadPolicy::PreferReplica; ///< 读路由策略
    };

    /**
     * @brief Redis 集群客户端
     * @details 支持 Redis Cluster 模式的命令路由、MOVED/ASK 重定向、
     *          槽位自动刷新和基于 CRC16 的键路由
     */
    class RedisClusterClient
    {
    public:
        /**
         * @brief 构造集群客户端
         * @param scheduler IO 调度器
         * @param config 异步 Redis 配置
         */
        explicit RedisClusterClient(IOScheduler* scheduler,
                                    AsyncRedisConfig config = AsyncRedisConfig::noTimeout());

        /**
         * @brief 构造集群客户端并保存拓扑配置
         * @param scheduler IO 调度器
         * @param config 异步 Redis 配置
         * @param retry_config 拓扑重试配置
         * @param refresh_config 拓扑刷新配置
         * @param read_policy 读路由策略
         */
        explicit RedisClusterClient(IOScheduler* scheduler,
                                    AsyncRedisConfig config,
                                    RedisTopologyRetryConfig retry_config,
                                    RedisTopologyRefreshConfig refresh_config,
                                    RedisReadPolicy read_policy);

        /**
         * @brief 添加集群节点
         * @param node 集群节点地址（含槽位范围）
         * @return 连接操作等待体
         */
        RedisConnectOperation addNode(const RedisClusterNodeAddress& node);

        /**
         * @brief 设置指定节点的槽位范围
         * @param node_index 节点索引
         * @param slot_start 槽位起始
         * @param slot_end 槽位结束
         */
        void setSlotRange(size_t node_index, uint16_t slot_start, uint16_t slot_end);

        /**
         * @brief 设置自动刷新槽位信息的间隔
         * @param interval 刷新间隔
         */
        void setAutoRefreshInterval(std::chrono::milliseconds interval);

        /**
         * @brief 执行 Redis 命令（支持自动路由和重试）
         * @param cmd 命令名称
         * @param args 命令参数
         * @param routing_key 路由键（为空时随机选择节点）
         * @param auto_retry 是否自动重试
         * @return 命令执行结果协程任务
         */
        Task<RedisCommandResult> execute(const std::string& cmd,
                                         const std::vector<std::string>& args,
                                         std::string routing_key = std::string(),
                                         bool auto_retry = true);

        /**
         * @brief 批量执行命令（Pipeline）
         * @param commands 命令视图数组
         * @param routing_key 路由键
         * @return 命令交换操作等待体
         */
        RedisExchangeOperation batch(std::span<const RedisCommandView> commands,
                                     std::string routing_key = std::string());

        /**
         * @brief 刷新集群槽位映射
         * @return 命令执行结果协程任务
         */
        Task<RedisCommandResult> refreshSlots();

        /**
         * @brief 计算键对应的槽位号
         * @param key Redis 键
         * @return 槽位号（0-16383）
         */
        uint16_t keySlot(const std::string& key) const;

        size_t nodeCount() const noexcept; ///< 获取节点数量
        /**
         * @brief 获取指定索引的节点客户端
         * @param index 节点索引
         * @return 节点客户端引用（若索引越界返回空）
         */
        std::optional<std::reference_wrapper<RedisClient<>>> node(size_t index);

        /**
         * @brief 获取拓扑统计信息
         * @return 当前节点、槽位缓存和拓扑配置快照
         */
        RedisTopologyStats getStats() const noexcept;

    private:
        /**
         * @brief 集群节点句柄
         */
        struct ClusterNode
        {
            RedisClusterNodeAddress address;                    ///< 节点地址（含槽位范围）
            std::unique_ptr<RedisClient<>> client;                ///< 客户端实例
            bool connected = false;                             ///< 连接状态
        };

        /**
         * @brief 重定向信息
         * @details 解析 MOVED/ASK 重定向响应后得到的目标节点信息
         */
        struct RedirectInfo
        {
            /**
             * @brief 重定向类型
             */
            enum class Type
            {
                None,   ///< 无重定向
                Moved,  ///< MOVED 重定向（永久）
                Ask,    ///< ASK 重定向（临时）
            };
            std::string host;        ///< 目标主机
            Type type = Type::None;  ///< 重定向类型
            int32_t port = 0;        ///< 目标端口
            uint16_t slot = 0;       ///< 目标槽位
        };

        Task<RedisCommandResult> refreshSlotsTask(); ///< 刷新槽位协程
        Task<RedisCommandResult> runAutoTask(std::string routing_key,
                                             std::string cmd,
                                             std::vector<std::string> args,
                                             bool force_key_routing,
                                             bool allow_auto_refresh,
                                             size_t max_attempts); ///< 自动重试执行协程

        static uint16_t crc16(const uint8_t* data, size_t len); ///< CRC16 校验和计算
        static std::string extractHashTag(const std::string& key); ///< 提取哈希标签
        static std::optional<RedirectInfo> parseRedirect(const RedisValue& value); ///< 解析重定向响应

        RedisClient<>* chooseNodeBySlot(uint16_t slot) noexcept; ///< 根据槽位选择节点
        RedisClient<>* chooseNodeByKey(const std::string& key) noexcept; ///< 根据键选择节点
        ClusterNode* chooseNodeHandleBySlot(uint16_t slot) noexcept; ///< 根据槽位选择节点句柄
        ClusterNode* chooseNodeHandleByKey(const std::string& key) noexcept; ///< 根据键选择节点句柄
        ClusterNode* findOrCreateNode(const std::string& host, int32_t port); ///< 查找或创建节点
        bool applyClusterSlots(const std::vector<RedisValue>& values, std::string* error_message); ///< 应用集群槽位信息
        bool shouldAutoRefresh() const noexcept; ///< 是否需要自动刷新

        IOScheduler* m_scheduler;                                     ///< IO 调度器
        AsyncRedisConfig m_config;                                    ///< 异步配置
        std::vector<ClusterNode> m_nodes;                             ///< 集群节点列表
        std::unique_ptr<RedisClient<>> m_fallback_client;               ///< 回退客户端
        std::array<int, 16384> m_slot_owner{};                        ///< 槽位到节点的映射表
        std::chrono::milliseconds m_auto_refresh_interval{5000};      ///< 自动刷新间隔
        std::chrono::steady_clock::time_point m_last_refresh_time{};  ///< 上次刷新时间
        RedisTopologyRetryConfig m_retry_config;                      ///< 拓扑重试配置
        RedisTopologyRefreshConfig m_refresh_config;                  ///< 拓扑刷新配置
        RedisReadPolicy m_read_policy = RedisReadPolicy::PreferReplica; ///< 读路由策略
        bool m_slot_cache_ready = false;                              ///< 槽位缓存是否就绪
    };

#ifdef GALAY_SSL_FEATURE_ENABLED
    class RedissMasterSlaveClient;

    /**
     * @brief Rediss（TLS）主从客户端构建器
     * @details 使用建造者模式逐步配置并构建 RedissMasterSlaveClient 实例
     */
    class RedissMasterSlaveClientBuilder
    {
    public:
        RedissMasterSlaveClientBuilder& scheduler(IOScheduler* scheduler) ///< 设置 IO 调度器
        {
            m_scheduler = scheduler;
            return *this;
        }

        RedissMasterSlaveClientBuilder& config(AsyncRedisConfig config) ///< 设置异步配置
        {
            m_config = std::move(config);
            return *this;
        }

        RedissMasterSlaveClientBuilder& tlsConfig(RedissClientConfig config) ///< 设置 TLS 配置
        {
            m_tls_config = std::move(config);
            return *this;
        }

        RedissMasterSlaveClientBuilder& sendTimeout(std::chrono::milliseconds timeout) ///< 设置发送超时
        {
            m_config.send_timeout = timeout;
            return *this;
        }

        RedissMasterSlaveClientBuilder& recvTimeout(std::chrono::milliseconds timeout) ///< 设置接收超时
        {
            m_config.recv_timeout = timeout;
            return *this;
        }

        RedissMasterSlaveClientBuilder& bufferSize(size_t size) ///< 设置缓冲区大小
        {
            m_config.buffer_size = size;
            return *this;
        }

        RedissMasterSlaveClientBuilder& retryConfig(RedisTopologyRetryConfig config) ///< 设置拓扑重试配置
        {
            m_retry_config = config;
            return *this;
        }

        RedissMasterSlaveClientBuilder& refreshConfig(RedisTopologyRefreshConfig config) ///< 设置拓扑刷新配置
        {
            m_refresh_config = config;
            return *this;
        }

        RedissMasterSlaveClientBuilder& readPolicy(RedisReadPolicy policy) ///< 设置读路由策略
        {
            m_read_policy = policy;
            return *this;
        }

        RedissMasterSlaveClient build() const; ///< 构建 Rediss 主从客户端

        AsyncRedisConfig buildConfig() const { return m_config; } ///< 获取当前配置
        RedissClientConfig buildTlsConfig() const { return m_tls_config; } ///< 获取 TLS 配置

    private:
        IOScheduler* m_scheduler = nullptr;
        AsyncRedisConfig m_config = AsyncRedisConfig::noTimeout();
        RedissClientConfig m_tls_config;
        RedisTopologyRetryConfig m_retry_config;
        RedisTopologyRefreshConfig m_refresh_config;
        RedisReadPolicy m_read_policy = RedisReadPolicy::PreferReplica;
    };

    /**
     * @brief Rediss（TLS）主从客户端
     * @details TLS 版本的 RedisMasterSlaveClient，支持主从读写分离和 Sentinel
     */
    class RedissMasterSlaveClient
    {
    public:
        /**
         * @brief 构造 Rediss 主从客户端
         * @param scheduler IO 调度器
         * @param config 异步 Redis 配置
         * @param tls_config TLS 配置
         */
        explicit RedissMasterSlaveClient(IOScheduler* scheduler,
                                         AsyncRedisConfig config = AsyncRedisConfig::noTimeout(),
                                         RedissClientConfig tls_config = {});

        /**
         * @brief 构造 Rediss 主从客户端并保存拓扑配置
         * @param scheduler IO 调度器
         * @param config 异步 Redis 配置
         * @param tls_config TLS 配置
         * @param retry_config 拓扑重试配置
         * @param refresh_config 拓扑刷新配置
         * @param read_policy 读路由策略
         */
        explicit RedissMasterSlaveClient(IOScheduler* scheduler,
                                         AsyncRedisConfig config,
                                         RedissClientConfig tls_config,
                                         RedisTopologyRetryConfig retry_config,
                                         RedisTopologyRefreshConfig refresh_config,
                                         RedisReadPolicy read_policy);

        detail::RedissConnectOperation connectMaster(const RedisNodeAddress& master); ///< 连接到主节点
        detail::RedissConnectOperation addReplica(const RedisNodeAddress& replica); ///< 添加从节点

        /**
         * @brief 执行 Redis 命令（支持读写分离和自动重试）
         * @param cmd 命令名称
         * @param args 命令参数
         * @param prefer_read 是否优先在读副本执行
         * @param auto_retry 是否自动重试
         * @return 命令执行结果协程任务
         */
        Task<RedisCommandResult> execute(const std::string& cmd,
                                         const std::vector<std::string>& args,
                                         bool prefer_read = false,
                                         bool auto_retry = true);

        /**
         * @brief 批量执行命令（Pipeline）
         * @param commands 命令视图数组
         * @param prefer_read 是否优先在读副本执行
         * @return 命令交换操作等待体
         */
        detail::RedissExchangeOperation batch(std::span<const RedisCommandView> commands,
                                              bool prefer_read = false);
        detail::RedissConnectOperation addSentinel(const RedisNodeAddress& sentinel); ///< 添加 Sentinel 节点
        void setSentinelMasterName(std::string master_name); ///< 设置 Sentinel 监控的主节点名称
        void setAutoRetryAttempts(size_t attempts) noexcept; ///< 设置自动重试次数
        Task<RedisCommandResult> refreshFromSentinel(); ///< 从 Sentinel 刷新主节点信息

        RedissClient& master(); ///< 获取主节点客户端引用
        std::optional<std::reference_wrapper<RedissClient>> replica(size_t index); ///< 获取从节点客户端
        size_t replicaCount() const noexcept; ///< 获取从节点数量
        RedisTopologyStats getStats() const noexcept; ///< 获取拓扑统计信息

    private:
        /**
         * @brief TLS 节点句柄
         */
        struct NodeHandle
        {
            RedisNodeAddress address;                          ///< 节点地址
            std::unique_ptr<RedissClient> client;              ///< TLS 客户端实例
            bool connected = false;                            ///< 连接状态
        };

        Task<RedisCommandResult> runAutoTask(bool prefer_read,
                                             std::string cmd,
                                             std::vector<std::string> args,
                                             size_t max_attempts); ///< 自动重试执行协程
        Task<RedisCommandResult> refreshSentinelTask(); ///< Sentinel 刷新协程

        bool isRetryableConnectionError(const RedisError& error) const noexcept; ///< 判断是否为可重试的连接错误
        RedissClient* chooseReadClient(); ///< 选择读客户端
        RedissClient* ensureMaster(); ///< 确保主节点可用
        bool parseMasterAddressReply(const std::vector<RedisValue>& values, RedisNodeAddress* out_addr) const; ///< 解析主节点地址回复
        bool parseReplicaListReply(const std::vector<RedisValue>& values, std::vector<RedisNodeAddress>* replicas) const; ///< 解析从节点列表回复

        IOScheduler* m_scheduler;                                     ///< IO 调度器
        AsyncRedisConfig m_config;                                    ///< 异步配置
        RedissClientConfig m_tls_config;                              ///< TLS 配置
        std::unique_ptr<RedissClient> m_master;                       ///< 主节点 TLS 客户端
        RedisNodeAddress m_master_address;                            ///< 主节点地址
        std::vector<std::unique_ptr<RedissClient>> m_replicas;        ///< 从节点 TLS 客户端列表
        std::vector<RedisNodeAddress> m_replica_addresses;            ///< 从节点地址列表
        std::vector<bool> m_replica_connected;                        ///< 从节点连接状态
        std::vector<NodeHandle> m_sentinels;                          ///< Sentinel 节点列表
        std::string m_sentinel_master_name = "mymaster";              ///< Sentinel 监控的主节点名称
        size_t m_read_cursor = 0;                                     ///< 读请求轮询游标
        size_t m_auto_retry_attempts = 2;                             ///< 自动重试次数
        RedisTopologyRetryConfig m_retry_config;                      ///< 拓扑重试配置
        RedisTopologyRefreshConfig m_refresh_config;                  ///< 拓扑刷新配置
        RedisReadPolicy m_read_policy = RedisReadPolicy::PreferReplica; ///< 读路由策略
        bool m_master_connected = false;                              ///< 主节点连接状态
    };

    class RedissClusterClient;

    /**
     * @brief Rediss（TLS）集群客户端构建器
     * @details 使用建造者模式逐步配置并构建 RedissClusterClient 实例
     */
    class RedissClusterClientBuilder
    {
    public:
        RedissClusterClientBuilder& scheduler(IOScheduler* scheduler) ///< 设置 IO 调度器
        {
            m_scheduler = scheduler;
            return *this;
        }

        RedissClusterClientBuilder& config(AsyncRedisConfig config) ///< 设置异步配置
        {
            m_config = std::move(config);
            return *this;
        }

        RedissClusterClientBuilder& tlsConfig(RedissClientConfig config) ///< 设置 TLS 配置
        {
            m_tls_config = std::move(config);
            return *this;
        }

        RedissClusterClientBuilder& sendTimeout(std::chrono::milliseconds timeout) ///< 设置发送超时
        {
            m_config.send_timeout = timeout;
            return *this;
        }

        RedissClusterClientBuilder& recvTimeout(std::chrono::milliseconds timeout) ///< 设置接收超时
        {
            m_config.recv_timeout = timeout;
            return *this;
        }

        RedissClusterClientBuilder& bufferSize(size_t size) ///< 设置缓冲区大小
        {
            m_config.buffer_size = size;
            return *this;
        }

        RedissClusterClientBuilder& retryConfig(RedisTopologyRetryConfig config) ///< 设置拓扑重试配置
        {
            m_retry_config = config;
            return *this;
        }

        RedissClusterClientBuilder& refreshConfig(RedisTopologyRefreshConfig config) ///< 设置拓扑刷新配置
        {
            m_refresh_config = config;
            return *this;
        }

        RedissClusterClientBuilder& readPolicy(RedisReadPolicy policy) ///< 设置读路由策略
        {
            m_read_policy = policy;
            return *this;
        }

        RedissClusterClient build() const; ///< 构建 Rediss 集群客户端

        AsyncRedisConfig buildConfig() const { return m_config; } ///< 获取当前配置
        RedissClientConfig buildTlsConfig() const { return m_tls_config; } ///< 获取 TLS 配置

    private:
        IOScheduler* m_scheduler = nullptr;
        AsyncRedisConfig m_config = AsyncRedisConfig::noTimeout();
        RedissClientConfig m_tls_config;
        RedisTopologyRetryConfig m_retry_config;
        RedisTopologyRefreshConfig m_refresh_config;
        RedisReadPolicy m_read_policy = RedisReadPolicy::PreferReplica;
    };

    /**
     * @brief Rediss（TLS）集群客户端
     * @details TLS 版本的 RedisClusterClient，支持集群命令路由、MOVED/ASK 重定向和槽位刷新
     */
    class RedissClusterClient
    {
    public:
        /**
         * @brief 构造 Rediss 集群客户端
         * @param scheduler IO 调度器
         * @param config 异步 Redis 配置
         * @param tls_config TLS 配置
         */
        explicit RedissClusterClient(IOScheduler* scheduler,
                                     AsyncRedisConfig config = AsyncRedisConfig::noTimeout(),
                                     RedissClientConfig tls_config = {});

        /**
         * @brief 构造 Rediss 集群客户端并保存拓扑配置
         * @param scheduler IO 调度器
         * @param config 异步 Redis 配置
         * @param tls_config TLS 配置
         * @param retry_config 拓扑重试配置
         * @param refresh_config 拓扑刷新配置
         * @param read_policy 读路由策略
         */
        explicit RedissClusterClient(IOScheduler* scheduler,
                                     AsyncRedisConfig config,
                                     RedissClientConfig tls_config,
                                     RedisTopologyRetryConfig retry_config,
                                     RedisTopologyRefreshConfig refresh_config,
                                     RedisReadPolicy read_policy);

        detail::RedissConnectOperation addNode(const RedisClusterNodeAddress& node); ///< 添加集群节点
        void setSlotRange(size_t node_index, uint16_t slot_start, uint16_t slot_end); ///< 设置槽位范围
        void setAutoRefreshInterval(std::chrono::milliseconds interval); ///< 设置自动刷新间隔

        /**
         * @brief 执行 Redis 命令（支持自动路由和重试）
         * @param cmd 命令名称
         * @param args 命令参数
         * @param routing_key 路由键
         * @param auto_retry 是否自动重试
         * @return 命令执行结果协程任务
         */
        Task<RedisCommandResult> execute(const std::string& cmd,
                                         const std::vector<std::string>& args,
                                         std::string routing_key = std::string(),
                                         bool auto_retry = true);

        /**
         * @brief 批量执行命令（Pipeline）
         * @param commands 命令视图数组
         * @param routing_key 路由键
         * @return 命令交换操作等待体
         */
        detail::RedissExchangeOperation batch(std::span<const RedisCommandView> commands,
                                              std::string routing_key = std::string());
        Task<RedisCommandResult> refreshSlots(); ///< 刷新集群槽位映射

        uint16_t keySlot(const std::string& key) const; ///< 计算键对应的槽位号
        size_t nodeCount() const noexcept; ///< 获取节点数量
        std::optional<std::reference_wrapper<RedissClient>> node(size_t index); ///< 获取指定节点客户端
        RedisTopologyStats getStats() const noexcept; ///< 获取拓扑统计信息

    private:
        /**
         * @brief TLS 集群节点句柄
         */
        struct ClusterNode
        {
            RedisClusterNodeAddress address;                    ///< 节点地址（含槽位范围）
            std::unique_ptr<RedissClient> client;               ///< TLS 客户端实例
            bool connected = false;                             ///< 连接状态
        };

        /**
         * @brief 重定向信息
         */
        struct RedirectInfo
        {
            /**
             * @brief 重定向类型
             */
            enum class Type
            {
                None,   ///< 无重定向
                Moved,  ///< MOVED 重定向（永久）
                Ask,    ///< ASK 重定向（临时）
            };
            std::string host;        ///< 目标主机
            Type type = Type::None;  ///< 重定向类型
            int32_t port = 0;        ///< 目标端口
            uint16_t slot = 0;       ///< 目标槽位
        };

        Task<RedisCommandResult> refreshSlotsTask(); ///< 刷新槽位协程
        Task<RedisCommandResult> runAutoTask(std::string routing_key,
                                             std::string cmd,
                                             std::vector<std::string> args,
                                             bool force_key_routing,
                                             bool allow_auto_refresh,
                                             size_t max_attempts); ///< 自动重试执行协程

        static uint16_t crc16(const uint8_t* data, size_t len); ///< CRC16 校验和计算
        static std::string extractHashTag(const std::string& key); ///< 提取哈希标签
        static std::optional<RedirectInfo> parseRedirect(const RedisValue& value); ///< 解析重定向响应

        RedissClient* chooseNodeBySlot(uint16_t slot) noexcept; ///< 根据槽位选择节点
        RedissClient* chooseNodeByKey(const std::string& key) noexcept; ///< 根据键选择节点
        ClusterNode* chooseNodeHandleBySlot(uint16_t slot) noexcept; ///< 根据槽位选择节点句柄
        ClusterNode* chooseNodeHandleByKey(const std::string& key) noexcept; ///< 根据键选择节点句柄
        ClusterNode* findOrCreateNode(const std::string& host, int32_t port); ///< 查找或创建节点
        bool applyClusterSlots(const std::vector<RedisValue>& values, std::string* error_message); ///< 应用集群槽位信息
        bool shouldAutoRefresh() const noexcept; ///< 是否需要自动刷新

        IOScheduler* m_scheduler;                                     ///< IO 调度器
        AsyncRedisConfig m_config;                                    ///< 异步配置
        RedissClientConfig m_tls_config;                              ///< TLS 配置
        std::vector<ClusterNode> m_nodes;                             ///< 集群节点列表
        std::unique_ptr<RedissClient> m_fallback_client;              ///< 回退客户端
        std::array<int, 16384> m_slot_owner{};                        ///< 槽位到节点的映射表
        std::chrono::milliseconds m_auto_refresh_interval{5000};      ///< 自动刷新间隔
        std::chrono::steady_clock::time_point m_last_refresh_time{};  ///< 上次刷新时间
        RedisTopologyRetryConfig m_retry_config;                      ///< 拓扑重试配置
        RedisTopologyRefreshConfig m_refresh_config;                  ///< 拓扑刷新配置
        RedisReadPolicy m_read_policy = RedisReadPolicy::PreferReplica; ///< 读路由策略
        bool m_slot_cache_ready = false;                              ///< 槽位缓存是否就绪
    };

#endif

    inline RedisMasterSlaveClient::RedisMasterSlaveClient(
        IOScheduler* scheduler,
        AsyncRedisConfig config,
        RedisTopologyRetryConfig retry_config,
        RedisTopologyRefreshConfig refresh_config,
        RedisReadPolicy read_policy)
        : m_scheduler(scheduler)
        , m_config(std::move(config))
        , m_auto_retry_attempts(retry_config.max_attempts == 0 ? 1 : retry_config.max_attempts)
        , m_retry_config(retry_config)
        , m_refresh_config(refresh_config)
        , m_read_policy(read_policy)
    {
        m_retry_config.max_attempts = m_auto_retry_attempts;
    }

    inline RedisTopologyStats RedisMasterSlaveClient::getStats() const noexcept
    {
        RedisTopologyStats stats;
        stats.master_count = m_master ? 1 : 0;
        stats.replica_count = m_replicas.size();
        stats.sentinel_count = m_sentinels.size();
        stats.master_connected = m_master_connected;
        stats.connected_node_count = m_master_connected ? 1 : 0;
        for (const bool connected : m_replica_connected) {
            if (connected) {
                ++stats.connected_node_count;
            }
        }
        for (const auto& sentinel : m_sentinels) {
            if (sentinel.connected) {
                ++stats.connected_node_count;
            }
        }
        stats.read_policy = m_read_policy;
        stats.retry_config = m_retry_config;
        stats.retry_config.max_attempts = m_auto_retry_attempts;
        stats.refresh_config = m_refresh_config;
        return stats;
    }

    inline RedisClusterClient::RedisClusterClient(
        IOScheduler* scheduler,
        AsyncRedisConfig config,
        RedisTopologyRetryConfig retry_config,
        RedisTopologyRefreshConfig refresh_config,
        RedisReadPolicy read_policy)
        : m_scheduler(scheduler)
        , m_config(std::move(config))
        , m_auto_refresh_interval(refresh_config.refresh_interval.count() > 0
                                      ? refresh_config.refresh_interval
                                      : std::chrono::milliseconds{5000})
        , m_retry_config(retry_config)
        , m_refresh_config(refresh_config)
        , m_read_policy(read_policy)
    {
        if (m_retry_config.max_attempts == 0) {
            m_retry_config.max_attempts = 1;
        }
        m_refresh_config.refresh_interval = m_auto_refresh_interval;
        m_slot_owner.fill(-1);
    }

    inline RedisTopologyStats RedisClusterClient::getStats() const noexcept
    {
        RedisTopologyStats stats;
        stats.cluster_node_count = m_nodes.size();
        for (const auto& node : m_nodes) {
            if (node.connected) {
                ++stats.connected_node_count;
            }
        }
        stats.slot_cache_ready = m_slot_cache_ready;
        stats.read_policy = m_read_policy;
        stats.retry_config = m_retry_config;
        stats.refresh_config = m_refresh_config;
        stats.refresh_config.refresh_interval = m_auto_refresh_interval;
        return stats;
    }

    inline galay::redis::RedisMasterSlaveClient galay::redis::RedisMasterSlaveClientBuilder::build() const
    {
        return RedisMasterSlaveClient(m_scheduler, m_config, m_retry_config, m_refresh_config, m_read_policy);
    }

    inline galay::redis::RedisClusterClient galay::redis::RedisClusterClientBuilder::build() const
    {
        return RedisClusterClient(m_scheduler, m_config, m_retry_config, m_refresh_config, m_read_policy);
    }

#ifdef GALAY_SSL_FEATURE_ENABLED
    inline RedissMasterSlaveClient::RedissMasterSlaveClient(
        IOScheduler* scheduler,
        AsyncRedisConfig config,
        RedissClientConfig tls_config,
        RedisTopologyRetryConfig retry_config,
        RedisTopologyRefreshConfig refresh_config,
        RedisReadPolicy read_policy)
        : m_scheduler(scheduler)
        , m_config(std::move(config))
        , m_tls_config(std::move(tls_config))
        , m_auto_retry_attempts(retry_config.max_attempts == 0 ? 1 : retry_config.max_attempts)
        , m_retry_config(retry_config)
        , m_refresh_config(refresh_config)
        , m_read_policy(read_policy)
    {
        m_retry_config.max_attempts = m_auto_retry_attempts;
    }

    inline RedisTopologyStats RedissMasterSlaveClient::getStats() const noexcept
    {
        RedisTopologyStats stats;
        stats.master_count = m_master ? 1 : 0;
        stats.replica_count = m_replicas.size();
        stats.sentinel_count = m_sentinels.size();
        stats.master_connected = m_master_connected;
        stats.connected_node_count = m_master_connected ? 1 : 0;
        for (const bool connected : m_replica_connected) {
            if (connected) {
                ++stats.connected_node_count;
            }
        }
        for (const auto& sentinel : m_sentinels) {
            if (sentinel.connected) {
                ++stats.connected_node_count;
            }
        }
        stats.read_policy = m_read_policy;
        stats.retry_config = m_retry_config;
        stats.retry_config.max_attempts = m_auto_retry_attempts;
        stats.refresh_config = m_refresh_config;
        return stats;
    }

    inline RedissClusterClient::RedissClusterClient(
        IOScheduler* scheduler,
        AsyncRedisConfig config,
        RedissClientConfig tls_config,
        RedisTopologyRetryConfig retry_config,
        RedisTopologyRefreshConfig refresh_config,
        RedisReadPolicy read_policy)
        : m_scheduler(scheduler)
        , m_config(std::move(config))
        , m_tls_config(std::move(tls_config))
        , m_auto_refresh_interval(refresh_config.refresh_interval.count() > 0
                                      ? refresh_config.refresh_interval
                                      : std::chrono::milliseconds{5000})
        , m_retry_config(retry_config)
        , m_refresh_config(refresh_config)
        , m_read_policy(read_policy)
    {
        if (m_retry_config.max_attempts == 0) {
            m_retry_config.max_attempts = 1;
        }
        m_refresh_config.refresh_interval = m_auto_refresh_interval;
        m_slot_owner.fill(-1);
    }

    inline RedisTopologyStats RedissClusterClient::getStats() const noexcept
    {
        RedisTopologyStats stats;
        stats.cluster_node_count = m_nodes.size();
        for (const auto& node : m_nodes) {
            if (node.connected) {
                ++stats.connected_node_count;
            }
        }
        stats.slot_cache_ready = m_slot_cache_ready;
        stats.read_policy = m_read_policy;
        stats.retry_config = m_retry_config;
        stats.refresh_config = m_refresh_config;
        stats.refresh_config.refresh_interval = m_auto_refresh_interval;
        return stats;
    }

    inline galay::redis::RedissMasterSlaveClient galay::redis::RedissMasterSlaveClientBuilder::build() const
    {
        return RedissMasterSlaveClient(m_scheduler,
                                       m_config,
                                       m_tls_config,
                                       m_retry_config,
                                       m_refresh_config,
                                       m_read_policy);
    }

    inline galay::redis::RedissClusterClient galay::redis::RedissClusterClientBuilder::build() const
    {
        return RedissClusterClient(m_scheduler,
                                   m_config,
                                   m_tls_config,
                                   m_retry_config,
                                   m_refresh_config,
                                   m_read_policy);
    }
#endif
}

#endif // GALAY_REDIS_TOPOLOGY_CLIENT_H
