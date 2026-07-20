#ifndef GALAY_MYSQL_DETAILS_AWAITABLE_H
#define GALAY_MYSQL_DETAILS_AWAITABLE_H

#include "../async/client.h"

namespace galay::mysql::details
{

// ======================== MysqlConnectAwaitable ========================

/**
 * @brief MySQL连接等待体
 * @details 通过最新 state-machine awaitable 内核执行 CONNECT -> READV -> SEND -> READV
 */
template<RingBufferBackendStrategy Strategy = RingBufferBackendStrategy::Mmap>
class MysqlConnectAwaitable
{
public:
    using Result = std::expected<std::optional<bool>, MysqlError>;

    /**
     * @brief 构造连接等待体
     * @param client 异步MySQL客户端引用
     * @param config MySQL连接配置
     */
    MysqlConnectAwaitable(AsyncMysqlClient<Strategy>& client, MysqlConfig config);
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
        explicit SharedState(AsyncMysqlClient<Strategy>& client, MysqlConfig config);

        AsyncMysqlClient<Strategy>* client = nullptr;           ///< 客户端指针
        MysqlConfig config;                                     ///< 连接配置
        galay::kernel::Host host;                               ///< 主机地址
        protocol::HandshakeV10 handshake;                       ///< 握手包数据
        std::string auth_plugin_name;                           ///< 当前认证插件名
        std::string auth_plugin_data;                           ///< 当前认证插件salt
        std::string auth_packet;                                ///< 认证数据包
        std::string parse_scratch;                              ///< 解析临时缓冲区
        std::array<struct iovec, 2> read_iovecs{};              ///< 读取iovec数组
        std::optional<Result> result;                           ///< 最终结果
        size_t sent = 0;                                        ///< 已发送字节数
        size_t read_iov_count = 0;                              ///< 读取iovec数量
        Phase phase = Phase::Connect;                           ///< 当前阶段
        AuthStage auth_stage = AuthStage::InitialResponse;      ///< 认证阶段
        bool connected = false;                                 ///< 是否已连接
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
template<RingBufferBackendStrategy Strategy = RingBufferBackendStrategy::Mmap>
class MysqlQueryAwaitable
    : public galay::kernel::TimeoutSupport<MysqlQueryAwaitable<Strategy>>
{
public:
    using Result = std::expected<std::optional<MysqlResultSet>, MysqlError>; ///< 查询结果类型

    /**
     * @brief 构造查询等待体
     * @param client 异步MySQL客户端引用
     * @param sql SQL查询语句
     */
    MysqlQueryAwaitable(AsyncMysqlClient<Strategy>& client, std::string_view sql);
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
        SharedState(AsyncMysqlClient<Strategy>& client, std::string_view sql);

        AsyncMysqlClient<Strategy>* client = nullptr;   ///< 客户端指针
        std::string encoded_cmd;                         ///< 编码后的命令
        MysqlResultSet result_set;                       ///< 结果集
        std::string parse_scratch;                       ///< 解析临时缓冲区
        std::array<struct iovec, 2> read_iovecs{};       ///< 读取iovec数组
        std::optional<Result> result;                    ///< 最终结果
        uint64_t column_count = 0;                       ///< 列数
        size_t sent = 0;                                 ///< 已发送字节数
        size_t columns_received = 0;                     ///< 已接收列数
        size_t read_iov_count = 0;                       ///< 读取iovec数量
        Phase phase = Phase::SendCommand;                ///< 当前阶段
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
template<RingBufferBackendStrategy Strategy = RingBufferBackendStrategy::Mmap>
class MysqlPrepareAwaitable
    : public galay::kernel::TimeoutSupport<MysqlPrepareAwaitable<Strategy>>
{
public:
    /**
     * @brief 预处理语句结果
     */
    struct PrepareResult {
        PrepareResult() = default;
        PrepareResult(uint32_t statement_id_value,
                      uint16_t num_columns_value,
                      uint16_t num_params_value) noexcept
            : statement_id(statement_id_value)
            , num_columns(num_columns_value)
            , num_params(num_params_value)
        {
        }
        PrepareResult(PrepareResult&&) noexcept = default;
        PrepareResult& operator=(PrepareResult&&) noexcept = default;

        /**
         * @brief 显式复制预处理结果
         * @return 拥有独立字段定义存储的新预处理结果
         */
        [[nodiscard]] PrepareResult clone() const
        {
            PrepareResult copy;
            copy.statement_id = statement_id;
            copy.num_columns = num_columns;
            copy.num_params = num_params;
            copy.param_fields.reserve(param_fields.size());
            for (const auto& field : param_fields) {
                copy.param_fields.push_back(field.clone());
            }
            copy.column_fields.reserve(column_fields.size());
            for (const auto& field : column_fields) {
                copy.column_fields.push_back(field.clone());
            }
            return copy;
        }

        uint32_t statement_id = 0;                  ///< 语句ID
        uint16_t num_columns = 0;                   ///< 列数
        uint16_t num_params = 0;                    ///< 参数数量
        std::vector<MysqlField> param_fields;       ///< 参数字段定义
        std::vector<MysqlField> column_fields;      ///< 结果列字段定义

    private:
        PrepareResult(const PrepareResult&) = delete;
        PrepareResult& operator=(const PrepareResult&) = delete;
    };
    using Result = std::expected<std::optional<PrepareResult>, MysqlError>; ///< 准备结果类型

    /**
     * @brief 构造预处理语句等待体
     * @param client 异步MySQL客户端引用
     * @param sql 预处理SQL语句
     */
    MysqlPrepareAwaitable(AsyncMysqlClient<Strategy>& client, std::string_view sql);
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
        SharedState(AsyncMysqlClient<Strategy>& client, std::string_view sql);

        AsyncMysqlClient<Strategy>* client = nullptr;   ///< 客户端指针
        std::string encoded_cmd;                         ///< 编码后的命令
        PrepareResult prepare_result;                    ///< 准备结果
        std::string parse_scratch;                       ///< 解析临时缓冲区
        std::array<struct iovec, 2> read_iovecs{};       ///< 读取iovec数组
        std::optional<Result> result;                    ///< 最终结果
        size_t sent = 0;                                 ///< 已发送字节数
        size_t params_received = 0;                      ///< 已接收参数数
        size_t columns_received = 0;                     ///< 已接收列数
        size_t read_iov_count = 0;                       ///< 读取iovec数量
        Phase phase = Phase::SendCommand;                ///< 当前阶段
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
template<RingBufferBackendStrategy Strategy = RingBufferBackendStrategy::Mmap>
class MysqlStmtExecuteAwaitable
    : public galay::kernel::TimeoutSupport<MysqlStmtExecuteAwaitable<Strategy>>
{
public:
    using Result = std::expected<std::optional<MysqlResultSet>, MysqlError>; ///< 执行结果类型

    /**
     * @brief 构造预处理语句执行等待体
     * @param client 异步MySQL客户端引用
     * @param encoded_cmd 已编码的命令数据
     */
    MysqlStmtExecuteAwaitable(AsyncMysqlClient<Strategy>& client, std::string encoded_cmd);
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
        SharedState(AsyncMysqlClient<Strategy>& client, std::string encoded_cmd);

        AsyncMysqlClient<Strategy>* client = nullptr;   ///< 客户端指针
        std::string encoded_cmd;                         ///< 已编码的命令
        MysqlResultSet result_set;                       ///< 结果集
        std::string parse_scratch;                       ///< 解析临时缓冲区
        std::array<struct iovec, 2> read_iovecs{};       ///< 读取iovec数组
        std::optional<Result> result;                    ///< 最终结果
        uint64_t column_count = 0;                       ///< 列数
        size_t sent = 0;                                 ///< 已发送字节数
        size_t columns_received = 0;                     ///< 已接收列数
        size_t read_iov_count = 0;                       ///< 读取iovec数量
        Phase phase = Phase::SendCommand;                ///< 当前阶段
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
template<RingBufferBackendStrategy Strategy = RingBufferBackendStrategy::Mmap>
class MysqlPipelineAwaitable
    : public galay::kernel::TimeoutSupport<MysqlPipelineAwaitable<Strategy>>
{
public:
    using Result = std::expected<std::optional<std::vector<MysqlResultSet>>, MysqlError>; ///< 流水线结果类型

    /**
     * @brief 构造Pipeline等待体
     * @param client 异步MySQL客户端引用
     * @param commands 编码后的命令视图数组
     */
    MysqlPipelineAwaitable(AsyncMysqlClient<Strategy>& client,
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
        SharedState(AsyncMysqlClient<Strategy>& client,
                    std::span<const protocol::MysqlCommandView> commands);

        AsyncMysqlClient<Strategy>* client = nullptr;           ///< 客户端指针
        size_t expected_results = 0;                            ///< 预期结果数量
        std::string encoded_buffer;                             ///< 编码缓冲区
        std::vector<EncodedSlice> encoded_slices;               ///< 命令切片列表
        std::vector<struct iovec> write_iovecs;                 ///< 写入iovec数组
        size_t write_iov_cursor = 0;                            ///< 写入iovec游标
        size_t next_command_index = 0;                          ///< 下一个待处理命令索引
        std::vector<MysqlResultSet> results;                    ///< 结果集合
        MysqlResultSet current_result;                          ///< 当前正在构建的结果集
        std::string parse_scratch;                              ///< 解析临时缓冲区
        std::array<struct iovec, 2> read_iovecs{};              ///< 读取iovec数组
        std::optional<Result> result;                           ///< 最终结果
        uint64_t column_count = 0;                              ///< 当前列数
        size_t columns_received = 0;                            ///< 已接收列数
        size_t read_iov_count = 0;                              ///< 读取iovec数量
        Phase phase = Phase::SendCommands;                      ///< 当前阶段
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

} // namespace galay::mysql::details

#endif // GALAY_MYSQL_DETAILS_AWAITABLE_H
