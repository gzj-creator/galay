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

#include <galay-kernel/async/tcp_socket.h>
#include <galay-kernel/core/awaitable.h>
#include <galay-kernel/core/io_scheduler.hpp>
#include <galay-kernel/core/task.h>
#include <galay-kernel/core/timeout.hpp>
#include <galay-kernel/common/host.hpp>
#include <galay-kernel/common/error.h>
#include <galay-utils/cache/ring_buffer.hpp>
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
#include "galay-mysql/base/mysql_error.h"
#include "galay-mysql/base/mysql_log.h"
#include "galay-mysql/base/mysql_value.h"
#include "galay-mysql/base/mysql_config.h"
#include "galay-mysql/protoc/mysql_protocol.h"
#include "galay-mysql/protoc/mysql_auth.h"
#include "galay-mysql/protoc/builder.h"

namespace galay::mysql
{

using galay::async::TcpSocket;
using galay::kernel::IOScheduler;
using galay::kernel::Host;
using galay::kernel::IOError;
using galay::kernel::IPType;
using galay::kernel::Task;


// 类型别名
using MysqlResult = std::expected<MysqlResultSet, MysqlError>;     ///< 查询结果类型
using MysqlVoidResult = std::expected<void, MysqlError>;           ///< 无返回值操作结果类型

// 前向声明
class AsyncMysqlClient;

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
     * @brief 构建AsyncMysqlClient实例
     * @return 配置完成的AsyncMysqlClient对象
     */
    AsyncMysqlClient build() const;

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

// ======================== MysqlConnectAwaitable ========================

/**
 * @brief MySQL连接等待体
 * @details 通过最新 state-machine awaitable 内核执行 CONNECT -> READV -> SEND -> READV
 */
class MysqlConnectAwaitable
{
public:
    using Result = std::expected<std::optional<bool>, MysqlError>;

    /**
     * @brief 构造连接等待体
     * @param client 异步MySQL客户端引用
     * @param config MySQL连接配置
     */
    MysqlConnectAwaitable(AsyncMysqlClient& client, MysqlConfig config);
    MysqlConnectAwaitable(MysqlConnectAwaitable&&) noexcept = default;
    MysqlConnectAwaitable& operator=(MysqlConnectAwaitable&&) noexcept = default;
    MysqlConnectAwaitable(const MysqlConnectAwaitable&) = delete;
    MysqlConnectAwaitable& operator=(const MysqlConnectAwaitable&) = delete;

    bool await_ready() { return m_inner.await_ready(); } ///< 检查是否已完成
    /**
     * @brief 挂起协程，等待连接完成
     * @param handle 协程句柄
     * @return 是否需要挂起
     */
    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> handle)
    {
        return m_inner.await_suspend(handle);
    }
    Result await_resume() { return m_inner.await_resume(); } ///< 获取连接结果

    /**
     * @brief 检查等待体是否无效（未正确初始化）
     * @return 无效时返回true
     */
    bool isInvalid() const;

private:
    /**
     * @brief 认证阶段枚举
     */
    enum class AuthStage {
        InitialResponse,      ///< 初始响应阶段
        AwaitFastAuthResult,  ///< 等待快速认证结果
        AwaitPublicKey,       ///< 等待服务器公钥
        AwaitFinalResult      ///< 等待最终认证结果
    };

    /**
     * @brief 连接阶段枚举
     */
    enum class Phase {
        Invalid,         ///< 无效状态
        Connect,         ///< TCP连接阶段
        HandshakeRead,   ///< 读取握手包阶段
        AuthWrite,       ///< 发送认证包阶段
        AuthResultRead,  ///< 读取认证结果阶段
        Done             ///< 完成
    };

    /**
     * @brief 连接等待体共享状态
     */
    struct SharedState {
        explicit SharedState(AsyncMysqlClient& client, MysqlConfig config);

        AsyncMysqlClient* client = nullptr;                     ///< 客户端指针
        MysqlConfig config;                                     ///< 连接配置
        galay::kernel::Host host;                               ///< 主机地址
        Phase phase = Phase::Connect;                           ///< 当前阶段
        protocol::HandshakeV10 handshake;                       ///< 握手包数据
        std::string auth_plugin_name;                           ///< 当前认证插件名
        std::string auth_plugin_data;                           ///< 当前认证插件salt
        AuthStage auth_stage = AuthStage::InitialResponse;      ///< 认证阶段
        std::string auth_packet;                                ///< 认证数据包
        size_t sent = 0;                                        ///< 已发送字节数
        bool connected = false;                                 ///< 是否已连接
        std::string parse_scratch;                              ///< 解析临时缓冲区
        std::array<struct iovec, 2> read_iovecs{};              ///< 读取iovec数组
        size_t read_iov_count = 0;                              ///< 读取iovec数量
        std::optional<Result> result;                           ///< 最终结果
    };

    /**
     * @brief 连接状态机
     */
    struct Machine {
        using result_type = Result; ///< 结果类型
        static constexpr galay::kernel::SequenceOwnerDomain kSequenceOwnerDomain =
            galay::kernel::SequenceOwnerDomain::ReadWrite;

        explicit Machine(std::shared_ptr<SharedState> state);

        /**
         * @brief 推进状态机执行
         * @return 状态机动作
         */
        galay::kernel::MachineAction<result_type> advance();
        void onConnect(std::expected<void, IOError> result);  ///< 连接完成回调
        void onRead(std::expected<size_t, IOError> result);   ///< 读取完成回调
        void onWrite(std::expected<size_t, IOError> result);  ///< 写入完成回调

    private:
        bool prepareReadWindow();                                              ///< 准备读取窗口
        std::expected<bool, MysqlError> parseHandshakeFromRingBuffer();        ///< 从环形缓冲区解析握手包
        std::expected<bool, MysqlError> parseAuthResultFromRingBuffer();       ///< 从环形缓冲区解析认证结果
        void setError(MysqlError error) noexcept;                              ///< 设置错误
        void setConnectError(const IOError& io_error) noexcept;                ///< 设置连接错误
        void setSendError(const IOError& io_error) noexcept;                   ///< 设置发送错误
        void setRecvError(const std::string& phase, const IOError& io_error) noexcept; ///< 设置接收错误
        void completeSuccess() noexcept;                                        ///< 标记成功完成

        std::shared_ptr<SharedState> m_state; ///< 共享状态
    };

    using InnerAwaitable = galay::kernel::StateMachineAwaitable<Machine>; ///< 内部状态机等待体类型

    std::shared_ptr<SharedState> m_state; ///< 共享状态
    InnerAwaitable m_inner;                ///< 内部等待体
};

// ============= MysqlQueryAwaitable ========================

/**
 * @brief MySQL查询等待体
 * @details 基于 sequence awaitable 链式执行 SEND -> READV，
 *          在“查询包发送完毕”和“结果集解析完毕”两个语义点唤醒。
 */
class MysqlQueryAwaitable
    : public galay::kernel::TimeoutSupport<MysqlQueryAwaitable>
{
public:
    using Result = std::expected<std::optional<MysqlResultSet>, MysqlError>; ///< 查询结果类型

    /**
     * @brief 构造查询等待体
     * @param client 异步MySQL客户端引用
     * @param sql SQL查询语句
     */
    MysqlQueryAwaitable(AsyncMysqlClient& client, std::string_view sql);
    MysqlQueryAwaitable(MysqlQueryAwaitable&&) noexcept = default;
    MysqlQueryAwaitable& operator=(MysqlQueryAwaitable&&) noexcept = default;
    MysqlQueryAwaitable(const MysqlQueryAwaitable&) = delete;
    MysqlQueryAwaitable& operator=(const MysqlQueryAwaitable&) = delete;

    bool await_ready() { return m_inner.await_ready(); } ///< 检查是否已完成
    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> handle) ///< 挂起协程
    {
        return m_inner.await_suspend(handle);
    }
    Result await_resume() { return m_inner.await_resume(); } ///< 获取查询结果
    void markTimeout() { m_inner.markTimeout(); }             ///< 标记超时

    bool isInvalid() const; ///< 检查等待体是否无效

private:
    /**
     * @brief 查询阶段枚举
     */
    enum class Phase {
        Invalid,           ///< 无效状态
        SendCommand,       ///< 发送命令阶段
        ReceivingHeader,   ///< 接收响应头阶段
        ReceivingColumns,  ///< 接收列定义阶段
        ReceivingColumnEof,///< 接收列定义结束标记阶段
        ReceivingRows,     ///< 接收行数据阶段
        Done               ///< 完成
    };

    /**
     * @brief 查询等待体共享状态
     */
    struct SharedState {
        SharedState(AsyncMysqlClient& client, std::string_view sql);

        AsyncMysqlClient* client = nullptr;             ///< 客户端指针
        std::string encoded_cmd;                         ///< 编码后的命令
        Phase phase = Phase::SendCommand;                ///< 当前阶段
        size_t sent = 0;                                 ///< 已发送字节数
        MysqlResultSet result_set;                       ///< 结果集
        uint64_t column_count = 0;                       ///< 列数
        size_t columns_received = 0;                     ///< 已接收列数
        std::string parse_scratch;                       ///< 解析临时缓冲区
        std::array<struct iovec, 2> read_iovecs{};       ///< 读取iovec数组
        size_t read_iov_count = 0;                       ///< 读取iovec数量
        std::optional<Result> result;                    ///< 最终结果
    };

    /**
     * @brief 查询状态机
     */
    struct Machine {
        using result_type = Result; ///< 结果类型
        static constexpr galay::kernel::SequenceOwnerDomain kSequenceOwnerDomain =
            galay::kernel::SequenceOwnerDomain::ReadWrite;

        explicit Machine(std::shared_ptr<SharedState> state);

        galay::kernel::MachineAction<result_type> advance(); ///< 推进状态机
        void onRead(std::expected<size_t, IOError> result);  ///< 读取完成回调
        void onWrite(std::expected<size_t, IOError> result); ///< 写入完成回调

    private:
        bool prepareReadWindow();                                        ///< 准备读取窗口
        std::expected<bool, MysqlError> tryParseFromRingBuffer();        ///< 尝试从环形缓冲区解析
        void setError(MysqlError error) noexcept;                        ///< 设置错误
        void setSendError(const IOError& io_error) noexcept;             ///< 设置发送错误
        void setRecvError(const IOError& io_error) noexcept;             ///< 设置接收错误

        std::shared_ptr<SharedState> m_state; ///< 共享状态
    };

    using InnerAwaitable = galay::kernel::StateMachineAwaitable<Machine>; ///< 内部状态机等待体类型

    std::shared_ptr<SharedState> m_state; ///< 共享状态
    InnerAwaitable m_inner;                ///< 内部等待体
};

// ======================== MysqlPrepareAwaitable ========================

/**
 * @brief MySQL预处理语句准备等待体
 * @details 发送COM_STMT_PREPARE并接收响应
 */
class MysqlPrepareAwaitable
    : public galay::kernel::TimeoutSupport<MysqlPrepareAwaitable>
{
public:
    /**
     * @brief 预处理语句结果
     */
    struct PrepareResult {
        uint32_t statement_id;                      ///< 语句ID
        uint16_t num_columns;                       ///< 列数
        uint16_t num_params;                        ///< 参数数量
        std::vector<MysqlField> param_fields;       ///< 参数字段定义
        std::vector<MysqlField> column_fields;      ///< 结果列字段定义
    };
    using Result = std::expected<std::optional<PrepareResult>, MysqlError>; ///< 准备结果类型

    /**
     * @brief 构造预处理语句等待体
     * @param client 异步MySQL客户端引用
     * @param sql 预处理SQL语句
     */
    MysqlPrepareAwaitable(AsyncMysqlClient& client, std::string_view sql);
    MysqlPrepareAwaitable(MysqlPrepareAwaitable&&) noexcept = default;
    MysqlPrepareAwaitable& operator=(MysqlPrepareAwaitable&&) noexcept = default;
    MysqlPrepareAwaitable(const MysqlPrepareAwaitable&) = delete;
    MysqlPrepareAwaitable& operator=(const MysqlPrepareAwaitable&) = delete;

    bool await_ready() { return m_inner.await_ready(); } ///< 检查是否已完成
    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> handle) ///< 挂起协程
    {
        return m_inner.await_suspend(handle);
    }
    Result await_resume() { return m_inner.await_resume(); } ///< 获取准备结果
    void markTimeout() { m_inner.markTimeout(); }              ///< 标记超时

    bool isInvalid() const; ///< 检查等待体是否无效

private:
    /**
     * @brief 预处理准备阶段枚举
     */
    enum class Phase {
        Invalid,              ///< 无效状态
        SendCommand,          ///< 发送命令阶段
        ReceivingPrepareOk,   ///< 接收准备OK包阶段
        ReceivingParamDefs,   ///< 接收参数定义阶段
        ReceivingParamEof,    ///< 接收参数结束标记阶段
        ReceivingColumnDefs,  ///< 接收列定义阶段
        ReceivingColumnEof,   ///< 接收列结束标记阶段
        Done                  ///< 完成
    };

    /**
     * @brief 预处理准备等待体共享状态
     */
    struct SharedState {
        SharedState(AsyncMysqlClient& client, std::string_view sql);

        AsyncMysqlClient* client = nullptr;             ///< 客户端指针
        std::string encoded_cmd;                         ///< 编码后的命令
        Phase phase = Phase::SendCommand;                ///< 当前阶段
        size_t sent = 0;                                 ///< 已发送字节数
        PrepareResult prepare_result;                    ///< 准备结果
        size_t params_received = 0;                      ///< 已接收参数数
        size_t columns_received = 0;                     ///< 已接收列数
        std::string parse_scratch;                       ///< 解析临时缓冲区
        std::array<struct iovec, 2> read_iovecs{};       ///< 读取iovec数组
        size_t read_iov_count = 0;                       ///< 读取iovec数量
        std::optional<Result> result;                    ///< 最终结果
    };

    /**
     * @brief 预处理准备状态机
     */
    struct Machine {
        using result_type = Result; ///< 结果类型
        static constexpr galay::kernel::SequenceOwnerDomain kSequenceOwnerDomain =
            galay::kernel::SequenceOwnerDomain::ReadWrite;

        explicit Machine(std::shared_ptr<SharedState> state);

        galay::kernel::MachineAction<result_type> advance(); ///< 推进状态机
        void onRead(std::expected<size_t, IOError> result);  ///< 读取完成回调
        void onWrite(std::expected<size_t, IOError> result); ///< 写入完成回调

    private:
        bool prepareReadWindow();                                        ///< 准备读取窗口
        std::expected<bool, MysqlError> tryParseFromRingBuffer();        ///< 尝试从环形缓冲区解析
        void setError(MysqlError error) noexcept;                        ///< 设置错误
        void setSendError(const IOError& io_error) noexcept;             ///< 设置发送错误
        void setRecvError(const IOError& io_error) noexcept;             ///< 设置接收错误

        std::shared_ptr<SharedState> m_state; ///< 共享状态
    };

    using InnerAwaitable = galay::kernel::StateMachineAwaitable<Machine>; ///< 内部状态机等待体类型

    std::shared_ptr<SharedState> m_state; ///< 共享状态
    InnerAwaitable m_inner;                ///< 内部等待体
};

// ======================== MysqlStmtExecuteAwaitable ========================

/**
 * @brief MySQL预处理语句执行等待体
 * @details 发送COM_STMT_EXECUTE并接收结果集
 */
class MysqlStmtExecuteAwaitable
    : public galay::kernel::TimeoutSupport<MysqlStmtExecuteAwaitable>
{
public:
    using Result = std::expected<std::optional<MysqlResultSet>, MysqlError>; ///< 执行结果类型

    /**
     * @brief 构造预处理语句执行等待体
     * @param client 异步MySQL客户端引用
     * @param encoded_cmd 已编码的命令数据
     */
    MysqlStmtExecuteAwaitable(AsyncMysqlClient& client, std::string encoded_cmd);
    MysqlStmtExecuteAwaitable(MysqlStmtExecuteAwaitable&&) noexcept = default;
    MysqlStmtExecuteAwaitable& operator=(MysqlStmtExecuteAwaitable&&) noexcept = default;
    MysqlStmtExecuteAwaitable(const MysqlStmtExecuteAwaitable&) = delete;
    MysqlStmtExecuteAwaitable& operator=(const MysqlStmtExecuteAwaitable&) = delete;

    bool await_ready() { return m_inner.await_ready(); } ///< 检查是否已完成
    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> handle) ///< 挂起协程
    {
        return m_inner.await_suspend(handle);
    }
    Result await_resume() { return m_inner.await_resume(); } ///< 获取执行结果
    void markTimeout() { m_inner.markTimeout(); }              ///< 标记超时

    bool isInvalid() const; ///< 检查等待体是否无效

private:
    /**
     * @brief 预处理语句执行阶段枚举
     */
    enum class Phase {
        Invalid,           ///< 无效状态
        SendCommand,       ///< 发送命令阶段
        ReceivingHeader,   ///< 接收响应头阶段
        ReceivingColumns,  ///< 接收列定义阶段
        ReceivingColumnEof,///< 接收列定义结束标记阶段
        ReceivingRows,     ///< 接收行数据阶段
        Done               ///< 完成
    };

    /**
     * @brief 预处理语句执行等待体共享状态
     */
    struct SharedState {
        SharedState(AsyncMysqlClient& client, std::string encoded_cmd);

        AsyncMysqlClient* client = nullptr;             ///< 客户端指针
        std::string encoded_cmd;                         ///< 已编码的命令
        Phase phase = Phase::SendCommand;                ///< 当前阶段
        size_t sent = 0;                                 ///< 已发送字节数
        MysqlResultSet result_set;                       ///< 结果集
        uint64_t column_count = 0;                       ///< 列数
        size_t columns_received = 0;                     ///< 已接收列数
        std::string parse_scratch;                       ///< 解析临时缓冲区
        std::array<struct iovec, 2> read_iovecs{};       ///< 读取iovec数组
        size_t read_iov_count = 0;                       ///< 读取iovec数量
        std::optional<Result> result;                    ///< 最终结果
    };

    /**
     * @brief 预处理语句执行状态机
     */
    struct Machine {
        using result_type = Result; ///< 结果类型
        static constexpr galay::kernel::SequenceOwnerDomain kSequenceOwnerDomain =
            galay::kernel::SequenceOwnerDomain::ReadWrite;

        explicit Machine(std::shared_ptr<SharedState> state);

        galay::kernel::MachineAction<result_type> advance(); ///< 推进状态机
        void onRead(std::expected<size_t, IOError> result);  ///< 读取完成回调
        void onWrite(std::expected<size_t, IOError> result); ///< 写入完成回调

    private:
        bool prepareReadWindow();                                        ///< 准备读取窗口
        std::expected<bool, MysqlError> tryParseFromRingBuffer();        ///< 尝试从环形缓冲区解析
        void setError(MysqlError error) noexcept;                        ///< 设置错误
        void setSendError(const IOError& io_error) noexcept;             ///< 设置发送错误
        void setRecvError(const IOError& io_error) noexcept;             ///< 设置接收错误

        std::shared_ptr<SharedState> m_state; ///< 共享状态
    };

    using InnerAwaitable = galay::kernel::StateMachineAwaitable<Machine>; ///< 内部状态机等待体类型

    std::shared_ptr<SharedState> m_state; ///< 共享状态
    InnerAwaitable m_inner;                ///< 内部等待体
};

// ======================== MysqlPipelineAwaitable ========================

/**
 * @brief MySQL Pipeline等待体
 * @details 批量发送编码后的COM_QUERY包并统一接收/解析响应
 */
class MysqlPipelineAwaitable
    : public galay::kernel::TimeoutSupport<MysqlPipelineAwaitable>
{
public:
    using Result = std::expected<std::optional<std::vector<MysqlResultSet>>, MysqlError>; ///< 流水线结果类型

    /**
     * @brief 构造Pipeline等待体
     * @param client 异步MySQL客户端引用
     * @param commands 编码后的命令视图数组
     */
    MysqlPipelineAwaitable(AsyncMysqlClient& client,
                           std::span<const protocol::MysqlCommandView> commands);
    MysqlPipelineAwaitable(MysqlPipelineAwaitable&&) noexcept = default;
    MysqlPipelineAwaitable& operator=(MysqlPipelineAwaitable&&) noexcept = default;
    MysqlPipelineAwaitable(const MysqlPipelineAwaitable&) = delete;
    MysqlPipelineAwaitable& operator=(const MysqlPipelineAwaitable&) = delete;

    bool await_ready() { return m_inner.await_ready(); } ///< 检查是否已完成
    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> handle) ///< 挂起协程
    {
        return m_inner.await_suspend(handle);
    }
    Result await_resume() { return m_inner.await_resume(); } ///< 获取流水线结果
    void markTimeout() { m_inner.markTimeout(); }              ///< 标记超时

    bool isInvalid() const; ///< 检查等待体是否无效

private:
    /**
     * @brief 流水线阶段枚举
     */
    enum class Phase {
        Invalid,           ///< 无效状态
        SendCommands,      ///< 发送命令阶段
        ReceivingHeader,   ///< 接收响应头阶段
        ReceivingColumns,  ///< 接收列定义阶段
        ReceivingColumnEof,///< 接收列定义结束标记阶段
        ReceivingRows,     ///< 接收行数据阶段
        Done               ///< 完成
    };

    /**
     * @brief 编码后的命令切片
     */
    struct EncodedSlice {
        size_t offset = 0; ///< 在编码缓冲区中的偏移量
        size_t length = 0; ///< 切片长度
    };

    /**
     * @brief Pipeline等待体共享状态
     */
    struct SharedState {
        SharedState(AsyncMysqlClient& client,
                    std::span<const protocol::MysqlCommandView> commands);

        AsyncMysqlClient* client = nullptr;                     ///< 客户端指针
        size_t expected_results = 0;                            ///< 预期结果数量
        std::string encoded_buffer;                             ///< 编码缓冲区
        std::vector<EncodedSlice> encoded_slices;               ///< 命令切片列表
        std::vector<struct iovec> write_iovecs;                 ///< 写入iovec数组
        size_t write_iov_cursor = 0;                            ///< 写入iovec游标
        size_t next_command_index = 0;                          ///< 下一个待处理命令索引
        Phase phase = Phase::SendCommands;                      ///< 当前阶段
        std::vector<MysqlResultSet> results;                    ///< 结果集合
        MysqlResultSet current_result;                          ///< 当前正在构建的结果集
        uint64_t column_count = 0;                              ///< 当前列数
        size_t columns_received = 0;                            ///< 已接收列数
        std::string parse_scratch;                              ///< 解析临时缓冲区
        std::array<struct iovec, 2> read_iovecs{};              ///< 读取iovec数组
        size_t read_iov_count = 0;                              ///< 读取iovec数量
        std::optional<Result> result;                           ///< 最终结果
    };

    /**
     * @brief Pipeline状态机
     */
    struct Machine {
        using result_type = Result; ///< 结果类型
        static constexpr galay::kernel::SequenceOwnerDomain kSequenceOwnerDomain =
            galay::kernel::SequenceOwnerDomain::ReadWrite;

        explicit Machine(std::shared_ptr<SharedState> state);

        galay::kernel::MachineAction<result_type> advance(); ///< 推进状态机
        void onRead(std::expected<size_t, IOError> result);  ///< 读取完成回调
        void onWrite(std::expected<size_t, IOError> result); ///< 写入完成回调

    private:
        bool prepareReadWindow();                                        ///< 准备读取窗口
        size_t pendingWriteIovCount();                                   ///< 获取待写入iovec数量
        bool advanceAfterWrite(size_t sent_bytes);                       ///< 写入后推进游标
        void refillWriteIovWindow();                                     ///< 重新填充写入iovec窗口
        void resetCurrentResult();                                       ///< 重置当前结果集
        void finalizeCurrentResult();                                    ///< 完成当前结果集
        std::expected<bool, MysqlError> tryParseFromRingBuffer();        ///< 尝试从环形缓冲区解析
        void setError(MysqlError error) noexcept;                        ///< 设置错误
        void setSendError(const IOError& io_error) noexcept;             ///< 设置发送错误
        void setRecvError(const IOError& io_error) noexcept;             ///< 设置接收错误

        std::shared_ptr<SharedState> m_state; ///< 共享状态
    };

    using InnerAwaitable = galay::kernel::StateMachineAwaitable<Machine>; ///< 内部状态机等待体类型

    std::shared_ptr<SharedState> m_state; ///< 共享状态
    InnerAwaitable m_inner;                ///< 内部等待体
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
class AsyncMysqlClient
{
public:
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
    MysqlConnectAwaitable connect(MysqlConfig config);

    /**
     * @brief 异步连接到MySQL服务器（参数形式）
     * @param host 服务器地址
     * @param port 服务器端口
     * @param user 用户名
     * @param password 密码
     * @param database 默认数据库
     * @return 连接等待体
     */
    MysqlConnectAwaitable connect(std::string_view host, uint16_t port,
                                  std::string_view user, std::string_view password,
                                  std::string_view database = "");

    // ======================== 查询 ========================

    /**
     * @brief 异步执行SQL查询
     * @param sql SQL语句
     * @return 查询等待体
     */
    MysqlQueryAwaitable query(std::string_view sql);

    /**
     * @brief 异步批量执行编码后的命令
     * @param commands 编码后的命令视图数组
     * @return Pipeline等待体
     */
    MysqlPipelineAwaitable batch(std::span<const protocol::MysqlCommandView> commands);

    /**
     * @brief 异步Pipeline执行多条SQL语句
     * @param sqls SQL语句数组
     * @return Pipeline等待体
     */
    MysqlPipelineAwaitable pipeline(std::span<const std::string_view> sqls);

    // ======================== 预处理语句 ========================

    /**
     * @brief 异步准备预处理语句
     * @param sql 预处理SQL语句
     * @return 准备等待体
     */
    MysqlPrepareAwaitable prepare(std::string_view sql);

    /**
     * @brief 异步执行预处理语句（string参数版本）
     * @param stmt_id 语句ID（由prepare返回）
     * @param params 参数值列表
     * @param param_types 参数类型列表
     * @return 执行等待体
     */
    MysqlStmtExecuteAwaitable stmtExecute(uint32_t stmt_id,
                                          std::span<const std::optional<std::string>> params,
                                          std::span<const uint8_t> param_types = {});

    /**
     * @brief 异步执行预处理语句（string_view参数版本）
     * @param stmt_id 语句ID（由prepare返回）
     * @param params 参数值列表
     * @param param_types 参数类型列表
     * @return 执行等待体
     */
    MysqlStmtExecuteAwaitable stmtExecute(uint32_t stmt_id,
                                          std::span<const std::optional<std::string_view>> params,
                                          std::span<const uint8_t> param_types = {});

    // ======================== 事务 ========================

    MysqlQueryAwaitable beginTransaction();  ///< 异步开启事务
    MysqlQueryAwaitable commit();             ///< 异步提交事务
    MysqlQueryAwaitable rollback();           ///< 异步回滚事务

    // ======================== 工具命令 ========================

    MysqlQueryAwaitable ping();                                ///< 异步发送心跳检测
    MysqlQueryAwaitable useDatabase(std::string_view database); ///< 异步切换数据库

    // ======================== 连接管理 ========================

    auto close() { m_is_closed = true; return m_socket.close(); } ///< 关闭连接
    bool isClosed() const { return m_is_closed; }                 ///< 检查连接是否已关闭

    // ======================== 内部访问 ========================

    TcpSocket& socket() { return m_socket; }                   ///< 获取TCP套接字引用
    galay::utils::RingBuffer& ringBuffer() { return m_ring_buffer; } ///< 获取接收环形缓冲区
    const galay::utils::RingBuffer& ringBuffer() const { return m_ring_buffer; } ///< 获取接收环形缓冲区
    protocol::MysqlParser& parser() { return m_parser; }       ///< 获取协议解析器引用
    protocol::MysqlEncoder& encoder() { return m_encoder; }    ///< 获取协议编码器引用
    const AsyncMysqlConfig& asyncConfig() const { return m_config; } ///< 获取异步配置
    uint32_t serverCapabilities() const { return m_server_capabilities; } ///< 获取服务器能力标志
    void setServerCapabilities(uint32_t caps) { m_server_capabilities = caps; } ///< 设置服务器能力标志
private:
    friend class MysqlConnectAwaitable;
    friend class MysqlQueryAwaitable;
    friend class MysqlPrepareAwaitable;
    friend class MysqlStmtExecuteAwaitable;
    friend class MysqlPipelineAwaitable;

    bool m_is_closed = false;                       ///< 是否已关闭
    TcpSocket m_socket;                             ///< TCP套接字
    IOScheduler* m_scheduler;                       ///< IO调度器指针
    protocol::MysqlParser m_parser;                 ///< 协议解析器
    protocol::MysqlEncoder m_encoder;               ///< 协议编码器
    AsyncMysqlConfig m_config;                      ///< 异步配置
    galay::utils::RingBuffer m_ring_buffer;         ///< 接收环形缓冲区
    uint32_t m_server_capabilities = 0;             ///< 服务器能力标志

};

/**
 * @brief AsyncMysqlClientBuilder::build的inline实现
 * @return 构建的AsyncMysqlClient对象
 */
inline galay::mysql::AsyncMysqlClient galay::mysql::AsyncMysqlClientBuilder::build() const
{
    return AsyncMysqlClient(m_scheduler, m_config);
}

} // namespace galay::mysql

#endif // GALAY_MYSQL_ASYNC_CLIENT_H
