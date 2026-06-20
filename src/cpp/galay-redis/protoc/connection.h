/**
 * @file connection.h
 * @brief Redis 同步 TCP 连接封装
 * @author galay-redis
 * @version 1.0.0
 *
 * @details 提供简单的 TCP 连接封装，用于同步 Redis 操作，
 *          包含连接建立、数据收发和 RESP 响应解析功能。
 */

#ifndef GALAY_REDIS_PROTOCOL_CONNECTION_H
#define GALAY_REDIS_PROTOCOL_CONNECTION_H

#include "redis_protocol.h"
#include "../base/redis_error.h"
#include <string>
#include <expected>
#include <vector>
#include <cstdint>

namespace galay::redis::protocol
{
    /**
     * @brief 简单的 TCP 连接封装，用于同步 Redis 操作
     * @details 提供连接、发送、接收和执行命令的同步接口
     */
    class Connection
    {
    public:
        Connection();
        ~Connection();

        /**
         * @brief 连接到 Redis 服务器
         * @param host 服务器地址
         * @param port 服务器端口
         * @param timeout_ms 连接超时时间（毫秒），默认 5000
         * @return 成功或错误
         */
        std::expected<void, RedisError> connect(const std::string& host, int port, uint32_t timeout_ms = 5000);

        /**
         * @brief 断开连接
         */
        void disconnect();

        /**
         * @brief 检查连接状态
         * @return 已连接返回 true
         */
        bool isConnected() const { return m_connected; }

        /**
         * @brief 发送数据
         * @param data 要发送的字符串数据
         * @return 成功或错误
         */
        std::expected<void, RedisError> send(const std::string& data);

        /**
         * @brief 接收并解析 Redis 响应
         * @return 解析后的 RedisReply 或错误
         */
        std::expected<RedisReply, RedisError> receiveReply();

        /**
         * @brief 发送命令并接收响应（便捷方法）
         * @param encoded_command 已编码的命令字符串
         * @return 解析后的 RedisReply 或错误
         */
        std::expected<RedisReply, RedisError> execute(const std::string& encoded_command);

    private:
        int m_socket_fd;                    ///< 套接字文件描述符
        bool m_connected;                   ///< 连接状态
        RespParser m_parser;                ///< RESP 协议解析器
        std::vector<char> m_recv_buffer;    ///< 接收缓冲区
        static constexpr size_t BUFFER_SIZE = 8192; ///< 默认缓冲区大小
    };
}

#endif
