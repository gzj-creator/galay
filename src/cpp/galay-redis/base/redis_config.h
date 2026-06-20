/**
 * @file redis_config.h
 * @brief Redis 连接配置
 * @author galay-redis
 * @version 1.0.0
 *
 * @details 定义同步 Redis 会话和异步 Redis 客户端共享的显式配置结构。
 */

#ifndef GALAY_REDIS_CONFIG_H
#define GALAY_REDIS_CONFIG_H

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace galay::redis
{
    /**
     * @brief Redis 同步会话连接配置
     */
    struct RedisSessionConfig
    {
        std::string host = "127.0.0.1";
        int32_t port = 6379;
        std::string username;
        std::string password;
        int32_t db_index = 0;
        int version = 2;
        uint32_t connect_timeout_ms = 5000;

        // Reserved for a future sync transport path. Current protocol::Connection
        // supports host/port/timeout only.
        std::optional<std::string> bind_address;
        bool reuse_address = false;
        std::optional<std::string> unix_socket_path;
    };

    /**
     * @brief 异步 Redis 超时和缓冲区配置
     */
    struct AsyncRedisConfig
    {
        std::chrono::milliseconds send_timeout = std::chrono::milliseconds(-1);
        std::chrono::milliseconds recv_timeout = std::chrono::milliseconds(-1);
        size_t buffer_size = 65536;

        bool isSendTimeoutEnabled() const
        {
            return send_timeout >= std::chrono::milliseconds(0);
        }

        bool isRecvTimeoutEnabled() const
        {
            return recv_timeout >= std::chrono::milliseconds(0);
        }

        static AsyncRedisConfig withTimeout(std::chrono::milliseconds send,
                                            std::chrono::milliseconds recv)
        {
            AsyncRedisConfig cfg;
            cfg.send_timeout = send;
            cfg.recv_timeout = recv;
            return cfg;
        }

        static AsyncRedisConfig withRecvTimeout(std::chrono::milliseconds recv)
        {
            AsyncRedisConfig cfg;
            cfg.recv_timeout = recv;
            return cfg;
        }

        static AsyncRedisConfig withSendTimeout(std::chrono::milliseconds send)
        {
            AsyncRedisConfig cfg;
            cfg.send_timeout = send;
            return cfg;
        }

        static AsyncRedisConfig noTimeout()
        {
            return {};
        }
    };
}

#endif
