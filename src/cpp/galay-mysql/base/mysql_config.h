/**
 * @file mysql_config.h
 * @brief MySQL连接配置
 * @author galay-mysql
 * @version 1.0.0
 *
 * @details 定义MySQL连接所需的主机、端口、用户名、密码、数据库等配置参数。
 */

#ifndef GALAY_MYSQL_CONFIG_H
#define GALAY_MYSQL_CONFIG_H

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

namespace galay::mysql
{

/**
 * @brief MySQL连接配置
 * @details 包含MySQL服务器连接所需的全部参数，支持默认值和工厂方法创建。
 */
struct MysqlConfig
{
    std::string host = "127.0.0.1";     ///< 服务器地址
    uint16_t port = 3306;               ///< 服务器端口
    std::string username;               ///< 用户名
    std::string password;               ///< 密码
    std::string database;               ///< 默认数据库
    std::string charset = "utf8mb4";    ///< 字符集
    uint32_t connect_timeout_ms = 5000; ///< 连接超时（毫秒）
    bool tcp_no_delay = true;           ///< 是否为连接 socket 启用 TCP_NODELAY

    /**
     * @brief 创建默认配置
     * @return 默认配置对象
     */
    static MysqlConfig defaultConfig()
    {
        return {};
    }

    /**
     * @brief 创建指定连接参数的配置
     * @param host 服务器地址
     * @param port 服务器端口
     * @param user 用户名
     * @param password 密码
     * @param database 默认数据库
     * @return 配置对象
     */
    static MysqlConfig create(const std::string& host, uint16_t port,
                              const std::string& user, const std::string& password,
                              const std::string& database = "")
    {
        MysqlConfig config;
        config.host = host;
        config.port = port;
        config.username = user;
        config.password = password;
        config.database = database;
        return config;
    }
};

struct AsyncMysqlConfig
{
    std::chrono::milliseconds send_timeout = std::chrono::milliseconds(-1);
    std::chrono::milliseconds recv_timeout = std::chrono::milliseconds(-1);
    size_t buffer_size = 16384;
    size_t result_row_reserve_hint = 0;
    bool tcp_no_delay = true; ///< 快捷 connect(host, ...) 使用的默认 TCP_NODELAY 策略

    bool isSendTimeoutEnabled() const
    {
        return send_timeout >= std::chrono::milliseconds(0);
    }

    bool isRecvTimeoutEnabled() const
    {
        return recv_timeout >= std::chrono::milliseconds(0);
    }

    static AsyncMysqlConfig withTimeout(std::chrono::milliseconds send,
                                        std::chrono::milliseconds recv)
    {
        AsyncMysqlConfig cfg;
        cfg.send_timeout = send;
        cfg.recv_timeout = recv;
        return cfg;
    }

    static AsyncMysqlConfig withRecvTimeout(std::chrono::milliseconds recv)
    {
        AsyncMysqlConfig cfg;
        cfg.recv_timeout = recv;
        return cfg;
    }

    static AsyncMysqlConfig withSendTimeout(std::chrono::milliseconds send)
    {
        AsyncMysqlConfig cfg;
        cfg.send_timeout = send;
        return cfg;
    }

    static AsyncMysqlConfig noTimeout()
    {
        return {};
    }
};

} // namespace galay::mysql

#endif // GALAY_MYSQL_CONFIG_H
