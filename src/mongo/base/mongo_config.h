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

namespace galay::mongo
{

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

    std::string host = kDefaultHost;               ///< 服务器地址
    uint16_t port = kDefaultPort;                  ///< 服务器端口

    std::string username;                          ///< 认证用户名（为空则跳过认证）
    std::string password;                          ///< 认证密码

    std::string database = kDefaultDatabase;            ///< 默认业务库
    std::string auth_database = kDefaultAuthDatabase;   ///< SCRAM 认证库，默认 admin
    std::string hello_database = kDefaultHelloDatabase; ///< hello 握手使用的数据库，默认 admin

    std::string app_name = kDefaultAppName;        ///< 客户端应用名称，用于 hello 握手

    bool tcp_nodelay = kDefaultTcpNoDelay;                  ///< 是否启用 TCP_NODELAY
    uint32_t connect_timeout_ms = kDefaultConnectTimeoutMs; ///< TCP 连接超时（毫秒）
    size_t recv_buffer_size = kDefaultRecvBufferSize;       ///< 同步连接接收缓冲区大小（字节）

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
