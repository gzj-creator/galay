/**
 * @file connection.h
 * @brief 同步 TCP 连接，负责与 MongoDB 服务端的底层通信
 * @author galay-mongo
 * @version 1.0.0
 *
 * @details 封装阻塞式 TCP 连接，提供 MongoDB 消息的发送与接收能力。
 * 使用 galay::utils::RingBuffer 作为接收缓冲区，支持基于消息头长度解析的完整消息接收。
 *
 * @note 不可拷贝，仅支持移动语义
 */

#ifndef GALAY_MONGO_PROTOCOL_CONNECTION_H
#define GALAY_MONGO_PROTOCOL_CONNECTION_H

#include "mongo_protocol.h"
#include "../base/mongo_error.h"
#include "../../galay-utils/cache/ring_buffer.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>

namespace galay::mongo
{
struct MongoConfig;
}

namespace galay::mongo::protocol
{

/**
 * @brief 同步 TCP 连接，负责与 MongoDB 服务端的底层通信
 * @details 提供阻塞式的连接建立、数据发送和消息接收功能。
 * 内部使用 RingBuffer 缓冲接收数据，支持按消息头长度自动拼接完整消息。
 *
 * @note 不可拷贝，仅支持移动语义
 */
class Connection
{
public:
    /**
     * @brief TCP 连接选项
     */
    struct ConnectOptions
    {
        ConnectOptions();

        std::string host;               ///< 服务器地址
        size_t recv_buffer_size;        ///< 接收缓冲区大小（字节）
        uint32_t timeout_ms;            ///< 连接超时（毫秒）
        uint16_t port;                  ///< 服务器端口
        bool tcp_nodelay;               ///< 是否启用 TCP_NODELAY

        /**
         * @brief 从 MongoConfig 创建连接选项
         * @param config MongoDB 连接配置
         * @return 对应的连接选项
         */
        static ConnectOptions fromMongoConfig(const ::galay::mongo::MongoConfig& config);
    };

    Connection();   ///< 默认构造，创建未连接的实例
    ~Connection();  ///< 析构函数，自动断开连接

    Connection(const Connection&) = delete;             ///< 禁用拷贝构造
    Connection& operator=(const Connection&) = delete;  ///< 禁用拷贝赋值

    Connection(Connection&& other) noexcept;             ///< 移动构造函数
    Connection& operator=(Connection&& other) noexcept;  ///< 移动赋值运算符

    /**
     * @brief 建立到 MongoDB 服务端的 TCP 连接
     * @param options 连接参数
     * @return 成功返回 void，失败返回 MongoError
     */
    std::expected<void, MongoError> connect(const ConnectOptions& options);

    /**
     * @brief 断开连接并关闭 socket
     */
    void disconnect();

    /**
     * @brief 判断当前是否已连接
     * @return 已连接时返回 true
     */
    bool isConnected() const { return m_connected; }

    /**
     * @brief 发送原始数据
     * @param data 要发送的二进制数据
     * @return 成功返回 void，失败返回 MongoError
     */
    std::expected<void, MongoError> send(const std::string& data);

    /**
     * @brief 接收指定长度的原始字节
     * @param expected_len 期望接收的字节数
     * @return 接收到的二进制数据，或 MongoError
     */
    std::expected<std::string, MongoError> recvBytes(size_t expected_len);

    /**
     * @brief 接收一条完整 MongoDB 消息的原始字节
     * @return 消息的原始二进制数据，或 MongoError
     */
    std::expected<std::string, MongoError> recvMessageRaw();

    /**
     * @brief 接收并解码一条完整 MongoDB 消息
     * @return 解码后的 MongoMessage，或 MongoError
     */
    std::expected<MongoMessage, MongoError> recvMessage();

    /**
     * @brief 返回底层 socket 文件描述符
     * @return socket fd
     */
    int fd() const { return m_socket_fd; }

private:
    std::expected<void, MongoError> ensureData(size_t n);  ///< 确保缓冲区中至少有 n 字节数据
    std::expected<void, MongoError> recvExact(char* buffer, size_t n); ///< 精确接收 n 字节到指定缓冲区
    void copyReadable(size_t offset, char* dst, size_t len) const;     ///< 从环形缓冲区拷贝可读数据
    std::string consumeToString(size_t len);                           ///< 消费并转换为字符串

    galay::utils::RingBuffer m_recv_ring;           ///< 接收环形缓冲区
    std::string m_decode_buffer;                    ///< 解码用的临时缓冲区
    int m_socket_fd;                                ///< 底层 socket 文件描述符
    bool m_connected;                               ///< 连接状态标志

    static constexpr size_t kDefaultBufferSize = 16384;           ///< 默认接收缓冲区大小
    static constexpr size_t kMaxMessageSize = 128 * 1024 * 1024;  ///< 最大消息长度（128 MB）
};

} // namespace galay::mongo::protocol

#endif // GALAY_MONGO_PROTOCOL_CONNECTION_H
