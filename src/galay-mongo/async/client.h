/**
 * @file client.h
 * @brief MongoDB 异步客户端，基于协程的非阻塞命令执行
 * @author galay-mongo
 * @version 1.0.0
 *
 * @details 提供基于 C++20 协程的异步 MongoDB 客户端，包括：
 * - AsyncMongoClientBuilder：建造者模式构造异步客户端
 * - AsyncMongoClient：支持单命令和 pipeline 批量命令的异步执行
 * - MongoPipelineResponse：pipeline 批量命令的单条响应封装
 *
 * @note 所有异步操作需在协程中通过 co_await 调用
 */

#ifndef GALAY_MONGO_ASYNC_CLIENT_H
#define GALAY_MONGO_ASYNC_CLIENT_H

#include <galay-kernel/async/tcp_socket.h>
#include <galay-kernel/common/error.h>
#include <galay-kernel/common/host.hpp>
#include <galay-kernel/core/io_scheduler.hpp>
#include <galay-kernel/core/task.h>
#include <galay-utils/cache/ring_buffer.hpp>

#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "galay-mongo/base/mongo_config.h"
#include "galay-mongo/base/mongo_error.h"
#include "galay-mongo/base/mongo_log.h"
#include "galay-mongo/base/mongo_value.h"
#include "galay-mongo/protoc/mongo_protocol.h"

namespace galay::mongo
{

using galay::async::TcpSocket;
using galay::kernel::IOError;
using galay::kernel::IOScheduler;
using galay::kernel::Task;

class AsyncMongoClient;
struct AsyncMongoClientInternals;

/**
 * @brief 异步 MongoDB 客户端的建造者
 * @details 使用链式调用配置 I/O 调度器、超时、缓冲区等参数，最终通过 build() 构造 AsyncMongoClient
 */
class AsyncMongoClientBuilder
{
public:
    /**
     * @brief 设置 I/O 调度器
     * @param scheduler 调度器指针
     * @return 当前建造者的引用
     */
    AsyncMongoClientBuilder& scheduler(IOScheduler* scheduler)
    {
        m_scheduler = scheduler;
        return *this;
    }

    /**
     * @brief 设置完整的异步客户端配置
     * @param config 异步客户端配置
     * @return 当前建造者的引用
     */
    AsyncMongoClientBuilder& config(AsyncMongoConfig config)
    {
        m_config = std::move(config);
        return *this;
    }

    /**
     * @brief 设置发送超时
     * @param timeout 发送超时时间
     * @return 当前建造者的引用
     */
    AsyncMongoClientBuilder& sendTimeout(std::chrono::milliseconds timeout)
    {
        m_config.send_timeout = timeout;
        return *this;
    }

    /**
     * @brief 设置接收超时
     * @param timeout 接收超时时间
     * @return 当前建造者的引用
     */
    AsyncMongoClientBuilder& recvTimeout(std::chrono::milliseconds timeout)
    {
        m_config.recv_timeout = timeout;
        return *this;
    }

    /**
     * @brief 设置接收环形缓冲区大小
     * @param size 缓冲区大小（字节）
     * @return 当前建造者的引用
     */
    AsyncMongoClientBuilder& bufferSize(size_t size)
    {
        m_config.buffer_size = size;
        return *this;
    }

    /**
     * @brief 设置 pipeline 模式下每条命令的预留编码字节数
     * @param reserve 每条命令预留字节数
     * @return 当前建造者的引用
     */
    AsyncMongoClientBuilder& pipelineReservePerCommand(size_t reserve)
    {
        m_config.pipeline_reserve_per_command = reserve;
        return *this;
    }

    /**
     * @brief 构造 AsyncMongoClient 实例
     * @return 配置完成的异步客户端
     */
    AsyncMongoClient build() const;

    /**
     * @brief 仅构造配置对象，不创建客户端
     * @return 当前配置的副本
     */
    AsyncMongoConfig buildConfig() const
    {
        return m_config;
    }

private:
    IOScheduler* m_scheduler = nullptr;                                ///< I/O 调度器指针
    AsyncMongoConfig m_config = AsyncMongoConfig::noTimeout();         ///< 异步客户端配置
};

/**
 * @brief pipeline 批量命令中单条命令的响应
 * @details 包含对应的请求 ID、响应文档或错误信息
 */
struct MongoPipelineResponse
{
    int32_t request_id = 0;                          ///< 对应的请求 ID
    std::optional<MongoReply> reply;                 ///< 响应文档（成功时有效）
    std::optional<MongoError> error;                 ///< 错误信息（失败时有效）

    bool ok() const { return reply.has_value(); }    ///< 判断该条命令是否成功
};

using MongoConnectAwaitable = Task<std::expected<bool, MongoError>>;                                    ///< 连接操作的协程返回类型
using MongoCommandAwaitable = Task<std::expected<MongoReply, MongoError>>;                              ///< 单命令操作的协程返回类型
using MongoPipelineAwaitable = Task<std::expected<std::vector<MongoPipelineResponse>, MongoError>>;    ///< pipeline 批量命令的协程返回类型

/**
 * @brief 异步 MongoDB 客户端
 * @details 基于 C++20 协程和 galay-kernel I/O 调度器的非阻塞 MongoDB 客户端。
 * 支持：
 * - 异步连接（含 SCRAM-SHA-256 认证）
 * - 单命令执行
 * - pipeline 批量命令执行
 *
 * @note 不可拷贝，仅支持移动语义
 * @note 所有异步操作需在协程中通过 co_await 调用
 */
class AsyncMongoClient
{
public:
    /**
     * @brief 构造异步 MongoDB 客户端
     * @param scheduler I/O 调度器指针
     * @param config 异步客户端配置
     */
    AsyncMongoClient(IOScheduler* scheduler,
                     AsyncMongoConfig config = AsyncMongoConfig::noTimeout());

    AsyncMongoClient(AsyncMongoClient&& other) noexcept;             ///< 移动构造函数
    AsyncMongoClient& operator=(AsyncMongoClient&& other) noexcept;  ///< 移动赋值运算符

    AsyncMongoClient(const AsyncMongoClient&) = delete;              ///< 禁用拷贝构造
    AsyncMongoClient& operator=(const AsyncMongoClient&) = delete;   ///< 禁用拷贝赋值

    ~AsyncMongoClient() = default;                                   ///< 析构函数

    /**
     * @brief 使用完整配置连接到 MongoDB（含认证）
     * @param config MongoDB 连接配置
     * @return 连接成功返回 true，失败返回 MongoError
     */
    MongoConnectAwaitable connect(MongoConfig config);

    /**
     * @brief 使用地址和端口快速连接（无认证）
     * @param host 服务器地址
     * @param port 服务器端口
     * @param database 默认数据库
     * @return 连接成功返回 true，失败返回 MongoError
     */
    MongoConnectAwaitable connect(std::string_view host,
                                  uint16_t port,
                                  std::string_view database = "admin");

    /**
     * @brief 执行任意 MongoDB 命令
     * @param database 目标数据库
     * @param command 命令文档
     * @return 响应文档或错误
     */
    MongoCommandAwaitable command(std::string database, MongoDocument command);

    /**
     * @brief 发送 ping 命令检测连接是否存活
     * @param database 目标数据库，默认 "admin"
     * @return 响应文档或错误
     */
    MongoCommandAwaitable ping(std::string database = "admin");

    /**
     * @brief 批量执行多条命令（pipeline 模式）
     * @param database 目标数据库
     * @param commands 命令文档数组
     * @return 所有命令的响应列表或错误
     */
    MongoPipelineAwaitable pipeline(std::string database,
                                    std::span<const MongoDocument> commands);

    /**
     * @brief 异步关闭连接
     * @return 关闭操作的 awaitable 对象
     */
    auto close()
    {
        m_is_closed = true;
        return m_socket.close();
    }

    bool isClosed() const { return m_is_closed; }  ///< 判断连接是否已关闭

    TcpSocket& socket() { return m_socket; }        ///< 获取底层 TCP socket 的可变引用
    galay::utils::RingBuffer& ringBuffer() { return m_ring_buffer; }   ///< 获取接收环形缓冲区
    const galay::utils::RingBuffer& ringBuffer() const { return m_ring_buffer; } ///< 获取接收环形缓冲区

    /**
     * @brief 分配下一个请求 ID（线程不安全）
     * @return 递增的请求 ID
     */
    int32_t nextRequestId();
private:
    friend struct AsyncMongoClientInternals;

    /**
     * @brief 预留一批连续的请求 ID
     * @param count 需要预留的数量
     * @return 起始请求 ID
     */
    int32_t reserveRequestIdBlock(size_t count);

    bool m_is_closed = true;                                         ///< 连接是否已关闭
    AsyncMongoConfig m_config = AsyncMongoConfig::noTimeout();       ///< 异步客户端配置
    TcpSocket m_socket;                                              ///< 底层 TCP socket
    galay::utils::RingBuffer m_ring_buffer;                          ///< 接收环形缓冲区
    std::string m_decode_scratch;                                    ///< 解码用的临时缓冲区
    std::string m_ping_template_db;                                  ///< ping 命令模板数据库名
    std::string m_ping_encoded_template;                             ///< 预编码的 ping 命令模板
    size_t m_pipeline_reserve_per_command = 96;                      ///< pipeline 每条命令预留编码字节数
    int32_t m_next_request_id = 1;                                   ///< 下一个可分配的请求 ID
};

inline AsyncMongoClient AsyncMongoClientBuilder::build() const
{
    return AsyncMongoClient(m_scheduler, m_config);
}

} // namespace galay::mongo

#endif // GALAY_MONGO_ASYNC_CLIENT_H
