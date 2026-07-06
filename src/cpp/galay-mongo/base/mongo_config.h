/**
 * @file mongo_config.h
 * @brief MongoDB 连接配置
 * @author galay-mongo
 * @version 1.0.0
 *
 * @details 定义 MongoConfig 结构体，封装 MongoDB 连接所需的全部参数，包括：
 * - 服务器地址与端口
 * - SCRAM-SHA-256 认证信息
 * - TCP 连接选项（TCP_NODELAY、连接超时）
 * - 接收缓冲区大小
 */

#ifndef GALAY_MONGO_CONFIG_H
#define GALAY_MONGO_CONFIG_H

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace galay::mongo
{

/**
 * @brief MongoDB 服务器端点
 * @details 用于 replica set seed list 和后续拓扑选择；host 不拥有外部缓冲区，始终保存为值。
 */
struct MongoEndpoint
{
    std::string host = "127.0.0.1"; ///< 服务器地址
    uint16_t port = 27017;          ///< 服务器端口
};

/**
 * @brief MongoDB 读偏好
 */
enum class MongoReadPreference
{
    kPrimary,            ///< 只选择 primary
    kPrimaryPreferred,   ///< 优先 primary，必要时 secondary
    kSecondary,          ///< 只选择 secondary
    kSecondaryPreferred, ///< 优先 secondary，必要时 primary
    kNearest,            ///< 选择延迟窗口内最近节点
};

/**
 * @brief MongoDB 重试策略配置
 * @details 仅描述调用者意图；实际重试边界由 retry policy 层决定。
 */
struct MongoRetryConfig
{
    uint32_t max_attempts = 1;     ///< 最大尝试次数，1 表示不重试
    bool retry_reads = false;      ///< 是否允许安全读重试
    bool retry_writes = false;     ///< 是否允许可证明安全的写重试
};

/**
 * @brief MongoDB 连接池配置
 */
struct MongoPoolConfig
{
    size_t min_size = 0;                                      ///< 每个服务器的最小连接数
    size_t max_size = 1;                                      ///< 每个服务器的最大连接数
    std::chrono::milliseconds wait_queue_timeout{5000};       ///< checkout 等待超时
    std::chrono::milliseconds max_idle_time{60000};           ///< 空闲连接最大保留时间
};

/**
 * @brief MongoDB 拓扑与 server selection 配置
 */
struct MongoTopologyConfig
{
    std::string replica_set_name;                             ///< 期望的 replica set 名称，空表示不校验
    std::chrono::milliseconds server_selection_timeout{30000}; ///< server selection 超时
    std::chrono::milliseconds local_threshold{15};             ///< nearest 延迟窗口
    MongoReadPreference read_preference = MongoReadPreference::kPrimary; ///< 默认 primary
};

/**
 * @brief MongoDB 连接配置，包含地址、认证、超时等参数
 * @details 支持通过工厂方法快速创建配置实例，也可直接修改成员变量
 */
struct MongoConfig
{
    static constexpr const char* kDefaultHost = "127.0.0.1";            ///< 默认服务器地址
    static constexpr uint16_t kDefaultPort = 27017;                     ///< 默认服务器端口
    static constexpr const char* kDefaultDatabase = "admin";            ///< 默认业务库
    static constexpr const char* kDefaultAuthDatabase = "admin";        ///< 默认 SCRAM 认证库
    static constexpr const char* kDefaultHelloDatabase = "admin";       ///< 默认 hello 握手数据库
    static constexpr const char* kDefaultAppName = "galay-mongo";       ///< 默认客户端应用名称
    static constexpr bool kDefaultTcpNoDelay = true;                    ///< 默认启用 TCP_NODELAY
    static constexpr uint32_t kDefaultConnectTimeoutMs = 5000;          ///< 默认 TCP 连接超时（毫秒）
    static constexpr size_t kDefaultRecvBufferSize = 16384;             ///< 默认同步连接接收缓冲区大小

    std::string host = kDefaultHost;                    ///< 服务器地址
    std::string username;                               ///< 认证用户名（为空则跳过认证）
    std::string password;                               ///< 认证密码
    std::string database = kDefaultDatabase;            ///< 默认业务库
    std::string auth_database = kDefaultAuthDatabase;   ///< SCRAM 认证库，默认 admin
    std::string hello_database = kDefaultHelloDatabase; ///< hello 握手使用的数据库，默认 admin
    std::string app_name = kDefaultAppName;             ///< 客户端应用名称，用于 hello 握手
    std::vector<MongoEndpoint> seeds;                   ///< replica set seed list；为空时使用 host/port 兼容路径
    MongoTopologyConfig topology;                       ///< 拓扑与 server selection 配置
    MongoPoolConfig pool;                               ///< 连接池配置
    MongoRetryConfig retry;                             ///< 重试策略配置
    size_t recv_buffer_size = kDefaultRecvBufferSize;   ///< 同步连接接收缓冲区大小（字节）
    uint32_t connect_timeout_ms = kDefaultConnectTimeoutMs; ///< TCP 连接超时（毫秒）
    uint16_t port = kDefaultPort;                       ///< 服务器端口
    bool tcp_nodelay = kDefaultTcpNoDelay;              ///< 是否启用 TCP_NODELAY

    /**
     * @brief 返回全部使用默认值的配置
     * @return 默认配置实例
     */
    static MongoConfig defaultConfig()
    {
        return {};
    }

    /**
     * @brief 快速创建指定地址和端口的配置
     * @param host     服务器地址
     * @param port     服务器端口
     * @param database 默认业务库，默认 "admin"
     * @return 配置好的 MongoConfig 实例
     */
    static MongoConfig create(const std::string& host,
                              uint16_t port,
                              const std::string& database = "admin")
    {
        MongoConfig config;
        config.host = host;
        config.port = port;
        config.database = database;
        if (config.auth_database.empty()) {
            config.auth_database = database;
        }
        return config;
    }
};

struct AsyncMongoConfig
{
    std::chrono::milliseconds send_timeout = std::chrono::milliseconds(-1);
    std::chrono::milliseconds recv_timeout = std::chrono::milliseconds(-1);
    size_t buffer_size = 16384;
    size_t pipeline_reserve_per_command = 96;

    bool isSendTimeoutEnabled() const
    {
        return send_timeout >= std::chrono::milliseconds(0);
    }

    bool isRecvTimeoutEnabled() const
    {
        return recv_timeout >= std::chrono::milliseconds(0);
    }

    static AsyncMongoConfig withTimeout(std::chrono::milliseconds send,
                                        std::chrono::milliseconds recv)
    {
        AsyncMongoConfig config;
        config.send_timeout = send;
        config.recv_timeout = recv;
        return config;
    }

    static AsyncMongoConfig noTimeout()
    {
        return {};
    }
};

} // namespace galay::mongo

#endif // GALAY_MONGO_CONFIG_H
