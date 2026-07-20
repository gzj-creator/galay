/**
 * @file client.h
 * @brief 异步MySQL客户端及协程等待体
 * @author galay-mysql
 * @version 1.0.0
 *
 * @details 定义了异步MySQL客户端(AsyncMysqlClient)及其构建器(AsyncMysqlClientBuilder)，
 *          以及基于C++20协程的各种等待体：连接(MysqlConnectAwaitable)、查询(MysqlQueryAwaitable)、
 *          预处理语句准备(MysqlPrepareAwaitable)、预处理语句执行(MysqlStmtExecuteAwaitable)、
 *          流水线(MysqlPipelineAwaitable)。
 *          所有异步接口返回自定义Awaitable值对象，可直接通过co_await使用。
 */

#ifndef GALAY_MYSQL_ASYNC_CLIENT_H
#define GALAY_MYSQL_ASYNC_CLIENT_H

#include "../../galay-kernel/async/tcp_socket.h"
#include "../../galay-kernel/core/awaitable.h"
#include "../../galay-kernel/core/io_scheduler.hpp"
#include "../../galay-kernel/core/task.h"
#include "../../galay-kernel/core/timeout.hpp"
#include "../../galay-kernel/common/host.hpp"
#include "../../galay-kernel/common/error.h"
#include "../../galay-utils/cache/ring_buffer.hpp"
#include <memory>
#include <string>
#include <string_view>
#include <span>
#include <array>
#include <expected>
#include <optional>
#include <vector>
#include <coroutine>
#include <utility>
#include <sys/uio.h>
#include "../base/mysql_error.h"
#include "../base/mysql_log.h"
#include "../base/mysql_value.h"
#include "../base/mysql_config.h"
#include "../protoc/mysql_protocol.h"
#include "../protoc/mysql_auth.h"
#include "../protoc/builder.h"

namespace galay::mysql
{

using galay::async::TcpSocket;
using galay::kernel::IOScheduler;
using galay::kernel::Host;
using galay::kernel::IOError;
using galay::kernel::IPType;
using galay::kernel::Task;
using galay::utils::RingBufferBackendStrategy;
using galay::utils::RingBuffer;


// 类型别名
using MysqlResult = std::expected<MysqlResultSet, MysqlError>;     ///< 查询结果类型
using MysqlVoidResult = std::expected<void, MysqlError>;           ///< 无返回值操作结果类型

// 前向声明
template<RingBufferBackendStrategy Strategy = RingBufferBackendStrategy::Mmap>
class AsyncMysqlClient;

namespace details
{
template<RingBufferBackendStrategy Strategy> class MysqlConnectAwaitable;
template<RingBufferBackendStrategy Strategy> class MysqlQueryAwaitable;
template<RingBufferBackendStrategy Strategy> class MysqlPrepareAwaitable;
template<RingBufferBackendStrategy Strategy> class MysqlStmtExecuteAwaitable;
template<RingBufferBackendStrategy Strategy> class MysqlPipelineAwaitable;
} // namespace details

template<RingBufferBackendStrategy Strategy = RingBufferBackendStrategy::Mmap>
using MysqlConnectAwaitable = details::MysqlConnectAwaitable<Strategy>;
template<RingBufferBackendStrategy Strategy = RingBufferBackendStrategy::Mmap>
using MysqlQueryAwaitable = details::MysqlQueryAwaitable<Strategy>;
template<RingBufferBackendStrategy Strategy = RingBufferBackendStrategy::Mmap>
using MysqlPrepareAwaitable = details::MysqlPrepareAwaitable<Strategy>;
template<RingBufferBackendStrategy Strategy = RingBufferBackendStrategy::Mmap>
using MysqlStmtExecuteAwaitable = details::MysqlStmtExecuteAwaitable<Strategy>;
template<RingBufferBackendStrategy Strategy = RingBufferBackendStrategy::Mmap>
using MysqlPipelineAwaitable = details::MysqlPipelineAwaitable<Strategy>;

/**
 * @brief 异步MySQL客户端构建器
 * @details 使用链式调用方式配置并构建AsyncMysqlClient实例。
 */
class AsyncMysqlClientBuilder
{
public:
    /**
     * @brief 设置IO调度器
     * @param scheduler IO调度器指针
     * @return 构建器引用，支持链式调用
     */
    AsyncMysqlClientBuilder& scheduler(IOScheduler* scheduler)
    {
        m_scheduler = scheduler;
        return *this;
    }

    /**
     * @brief 设置异步配置
     * @param config 异步配置对象
     * @return 构建器引用，支持链式调用
     */
    AsyncMysqlClientBuilder& config(AsyncMysqlConfig config)
    {
        m_config = std::move(config);
        return *this;
    }

    /**
     * @brief 设置发送超时
     * @param timeout 超时时长（毫秒）
     * @return 构建器引用，支持链式调用
     */
    AsyncMysqlClientBuilder& sendTimeout(std::chrono::milliseconds timeout)
    {
        m_config.send_timeout = timeout;
        return *this;
    }

    /**
     * @brief 设置接收超时
     * @param timeout 超时时长（毫秒）
     * @return 构建器引用，支持链式调用
     */
    AsyncMysqlClientBuilder& recvTimeout(std::chrono::milliseconds timeout)
    {
        m_config.recv_timeout = timeout;
        return *this;
    }

    /**
     * @brief 设置缓冲区大小
     * @param size 缓冲区大小（字节）
     * @return 构建器引用，支持链式调用
     */
    AsyncMysqlClientBuilder& bufferSize(size_t size)
    {
        m_config.buffer_size = size;
        return *this;
    }

    /**
     * @brief 设置结果集行预分配提示
     * @param hint 预分配行数提示
     * @return 构建器引用，支持链式调用
     */
    AsyncMysqlClientBuilder& resultRowReserveHint(size_t hint)
    {
        m_config.result_row_reserve_hint = hint;
        return *this;
    }

    /**
     * @brief 设置快捷连接接口是否启用 TCP_NODELAY
     * @param enabled true 表示连接 socket 启用 TCP_NODELAY；false 表示保留系统默认
     * @return 构建器引用，支持链式调用
     * @note 显式传入 MysqlConfig 的 connect(MysqlConfig) 以 MysqlConfig::tcp_no_delay 为准。
     */
    AsyncMysqlClientBuilder& tcpNoDelay(bool enabled)
    {
        m_config.tcp_no_delay = enabled;
        return *this;
    }

    /**
     * @brief 构建AsyncMysqlClient实例
     * @return 配置完成的AsyncMysqlClient对象
     */
    AsyncMysqlClient<> build() const;

    /**
     * @brief 仅构建配置对象
     * @return 当前的AsyncMysqlConfig配置
     */
    AsyncMysqlConfig buildConfig() const
    {
        return m_config;
    }

private:
    IOScheduler* m_scheduler = nullptr;                                 ///< IO调度器指针
    AsyncMysqlConfig m_config = AsyncMysqlConfig::noTimeout();          ///< 异步配置
};

// ======================== AsyncMysqlClient ========================

/**
 * @brief 异步MySQL客户端
 * @details 所有异步接口返回自定义Awaitable值对象（而非Task<void>）
 *
 * @code
 * Task<void> testMysql(IOScheduler* scheduler) {
 *     AsyncMysqlClient client(scheduler);
 *     auto config = MysqlConfig::create("127.0.0.1", 3306, "root", "password", "test_db");
 *     auto connect_result = co_await client.connect(config);
 *     if (!connect_result) { co_return; }
 *
 *     auto result = co_await client.query("SELECT * FROM users");
 *     // 处理结果...
 *
 *     co_await client.close();
 * }
 * @endcode
 */
template<RingBufferBackendStrategy Strategy>
class AsyncMysqlClient
{
public:
    using ConnectAwaitable = details::MysqlConnectAwaitable<Strategy>;
    using QueryAwaitable = details::MysqlQueryAwaitable<Strategy>;
    using PrepareAwaitable = details::MysqlPrepareAwaitable<Strategy>;
    using StmtExecuteAwaitable = details::MysqlStmtExecuteAwaitable<Strategy>;
    using PipelineAwaitable = details::MysqlPipelineAwaitable<Strategy>;

    /**
     * @brief 构造异步MySQL客户端
     * @param scheduler IO调度器指针
     * @param config 异步配置（超时、缓冲区大小等）
     */
    AsyncMysqlClient(IOScheduler* scheduler,
                     AsyncMysqlConfig config = AsyncMysqlConfig::noTimeout());

    AsyncMysqlClient(AsyncMysqlClient&& other) noexcept;             ///< 移动构造
    AsyncMysqlClient& operator=(AsyncMysqlClient&& other) noexcept;  ///< 移动赋值

    AsyncMysqlClient(const AsyncMysqlClient&) = delete;              ///< 禁止拷贝构造
    AsyncMysqlClient& operator=(const AsyncMysqlClient&) = delete;   ///< 禁止拷贝赋值

    ~AsyncMysqlClient() = default;

    // ======================== 连接 ========================

    /**
     * @brief 异步连接到MySQL服务器
     * @param config MySQL连接配置
     * @return 连接等待体
     */
    MysqlConnectAwaitable<Strategy> connect(MysqlConfig config);

    /**
     * @brief 异步连接到MySQL服务器（参数形式）
     * @param host 服务器地址
     * @param port 服务器端口
     * @param user 用户名
     * @param password 密码
     * @param database 默认数据库
     * @return 连接等待体
     */
    MysqlConnectAwaitable<Strategy> connect(std::string_view host, uint16_t port,
                                            std::string_view user, std::string_view password,
                                            std::string_view database = "");

    // ======================== 查询 ========================

    /**
     * @brief 异步执行SQL查询
     * @param sql SQL语句
     * @return 查询等待体
     */
    MysqlQueryAwaitable<Strategy> query(std::string_view sql);

    /**
     * @brief 异步批量执行编码后的命令
     * @param commands 编码后的命令视图数组
     * @return Pipeline等待体
     */
    MysqlPipelineAwaitable<Strategy> batch(std::span<const protocol::MysqlCommandView> commands);

    /**
     * @brief 异步Pipeline执行多条SQL语句
     * @param sqls SQL语句数组
     * @return Pipeline等待体
     */
    MysqlPipelineAwaitable<Strategy> pipeline(std::span<const std::string_view> sqls);

    // ======================== 预处理语句 ========================

    /**
     * @brief 异步准备预处理语句
     * @param sql 预处理SQL语句
     * @return 准备等待体
     */
    MysqlPrepareAwaitable<Strategy> prepare(std::string_view sql);

    /**
     * @brief 异步执行预处理语句（string参数版本）
     * @param stmt_id 语句ID（由prepare返回）
     * @param params 参数值列表
     * @param param_types 参数类型列表
     * @return 执行等待体
     */
    MysqlStmtExecuteAwaitable<Strategy> stmtExecute(uint32_t stmt_id,
                                                    std::span<const std::optional<std::string>> params,
                                                    std::span<const uint8_t> param_types = {});

    /**
     * @brief 异步执行预处理语句（string_view参数版本）
     * @param stmt_id 语句ID（由prepare返回）
     * @param params 参数值列表
     * @param param_types 参数类型列表
     * @return 执行等待体
     */
    MysqlStmtExecuteAwaitable<Strategy> stmtExecute(uint32_t stmt_id,
                                                    std::span<const std::optional<std::string_view>> params,
                                                    std::span<const uint8_t> param_types = {});

    // ======================== 事务 ========================

    MysqlQueryAwaitable<Strategy> beginTransaction();  ///< 异步开启事务
    MysqlQueryAwaitable<Strategy> commit();             ///< 异步提交事务
    MysqlQueryAwaitable<Strategy> rollback();           ///< 异步回滚事务

    // ======================== 工具命令 ========================

    MysqlQueryAwaitable<Strategy> ping();                                ///< 异步发送心跳检测
    MysqlQueryAwaitable<Strategy> useDatabase(std::string_view database); ///< 异步切换数据库

    // ======================== 连接管理 ========================

    auto close() { m_is_closed = true; return m_socket.close(); } ///< 关闭连接
    bool isClosed() const { return m_is_closed; }                 ///< 检查连接是否已关闭

    // ======================== 内部访问 ========================

    TcpSocket& socket() { return m_socket; }                   ///< 获取TCP套接字引用
    RingBuffer<Strategy>& ringBuffer() { return m_ring_buffer; } ///< 获取接收环形缓冲区
    const RingBuffer<Strategy>& ringBuffer() const { return m_ring_buffer; } ///< 获取接收环形缓冲区
    protocol::MysqlParser& parser() { return m_parser; }       ///< 获取协议解析器引用
    protocol::MysqlEncoder& encoder() { return m_encoder; }    ///< 获取协议编码器引用
    const AsyncMysqlConfig& asyncConfig() const { return m_config; } ///< 获取异步配置
    uint32_t serverCapabilities() const { return m_server_capabilities; } ///< 获取服务器能力标志
    void setServerCapabilities(uint32_t caps) { m_server_capabilities = caps; } ///< 设置服务器能力标志
private:
    friend class details::MysqlConnectAwaitable<Strategy>;
    friend class details::MysqlQueryAwaitable<Strategy>;
    friend class details::MysqlPrepareAwaitable<Strategy>;
    friend class details::MysqlStmtExecuteAwaitable<Strategy>;
    friend class details::MysqlPipelineAwaitable<Strategy>;

    TcpSocket m_socket;                             ///< TCP套接字
    IOScheduler* m_scheduler;                       ///< IO调度器指针
    AsyncMysqlConfig m_config;                      ///< 异步配置
    RingBuffer<Strategy> m_ring_buffer;             ///< 接收环形缓冲区
    uint32_t m_server_capabilities = 0;             ///< 服务器能力标志
    protocol::MysqlParser m_parser;                 ///< 协议解析器
    protocol::MysqlEncoder m_encoder;               ///< 协议编码器
    bool m_is_closed = false;                       ///< 是否已关闭

};

/**
 * @brief AsyncMysqlClientBuilder::build的inline实现
 * @return 构建的AsyncMysqlClient对象
 */
inline galay::mysql::AsyncMysqlClient<> galay::mysql::AsyncMysqlClientBuilder::build() const
{
    return AsyncMysqlClient<>(m_scheduler, m_config);
}

} // namespace galay::mysql

#include "../details/awaitable.h"

#endif // GALAY_MYSQL_ASYNC_CLIENT_H
