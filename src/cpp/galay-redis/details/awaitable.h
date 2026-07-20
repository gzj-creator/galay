#ifndef GALAY_REDIS_DETAILS_AWAITABLE_H
#define GALAY_REDIS_DETAILS_AWAITABLE_H

/**
 * @file awaitable.h
 * @brief Redis/Rediss 客户端状态机等待体声明
 * @details 仅由 async/redis_client.h 在 galay::redis 命名空间内包含。
 */

    /**
     * @brief 内部实现细节命名空间
     * @details 包含命令交换和连接建立的状态机与共享状态
     */
    namespace detail
    {
        using RedisExchangeResult =
            std::expected<std::optional<std::vector<RedisValue>>, RedisError>;

        /**
         * @brief Redis 命令交换共享状态
         * @details 保存单次命令发送/接收过程中所有中间状态和缓冲区
         */
        template<galay::utils::RingBufferBackendStrategy Strategy>
        struct RedisExchangeSharedState
        {
            /**
             * @brief 交换阶段枚举
             */
            enum class Phase : uint8_t {
                Invalid, ///< 无效状态
                Start,   ///< 起始状态
                Send,    ///< 发送中
                Parse,   ///< 解析中
                Done     ///< 完成
            };

            /**
             * @brief 从已编码字符串构造（移动语义）
             */
            RedisExchangeSharedState(RedisClient<Strategy>& client,
                                     std::string encoded_command,
                                     size_t expected_replies,
                                     bool recv_only);
            /**
             * @brief 从字符串视图构造（零拷贝）
             */
            RedisExchangeSharedState(RedisClient<Strategy>& client,
                                     std::string_view encoded_command,
                                     size_t expected_replies,
                                     bool recv_only);
            /**
             * @brief 从批量命令视图构造
             */
            RedisExchangeSharedState(RedisClient<Strategy>& client,
                                     std::span<const RedisCommandView> commands);

            std::string encoded_cmd;                    ///< 已编码的命令字符串
            std::string parse_buffer;                   ///< 解析缓冲区
            std::vector<RedisValue> values;             ///< 解析得到的值
            std::array<struct iovec, 2> read_iovecs{};  ///< 读取 iovec 数组
            std::optional<RedisExchangeResult> result;  ///< 交换结果
            std::string_view encoded_view;              ///< 已编码的命令视图
            RedisClient<Strategy>* client = nullptr;    ///< 关联的客户端
            size_t expected_replies = 0;                ///< 期望的回复数量
            size_t sent = 0;                            ///< 已发送字节数
            size_t read_iov_count = 0;                  ///< 读取 iovec 数量
            Phase phase = Phase::Start;                 ///< 当前阶段
            bool recv_only = false;                     ///< 是否仅接收模式
        };

        /**
         * @brief Redis 命令交换状态机
         * @details 驱动命令发送和回复解析的异步状态机
         */
        template<galay::utils::RingBufferBackendStrategy Strategy>
        struct RedisExchangeMachine
        {
            using result_type = RedisExchangeResult;
            static constexpr galay::kernel::SequenceOwnerDomain kSequenceOwnerDomain =
                galay::kernel::SequenceOwnerDomain::ReadWrite;

            /**
             * @brief 构造交换状态机
             * @param state 共享状态指针
             */
            explicit RedisExchangeMachine(std::shared_ptr<RedisExchangeSharedState<Strategy>> state);

            /**
             * @brief 推进状态机
             * @return 状态机动作
             */
            galay::kernel::MachineAction<result_type> advance();

            /**
             * @brief 读取完成回调
             * @param result 读取结果
             */
            void onRead(std::expected<size_t, IOError> result);

            /**
             * @brief 写入完成回调
             * @param result 写入结果
             */
            void onWrite(std::expected<size_t, IOError> result);

        private:
            bool prepareReadWindow();                                   ///< 准备读取窗口
            std::expected<bool, RedisError> tryParseReplies();         ///< 尝试解析回复
            void setError(RedisError error) noexcept;                  ///< 设置 Redis 错误
            void setSendError(const IOError& io_error) noexcept;       ///< 设置发送错误
            void setRecvError(const IOError& io_error) noexcept;       ///< 设置接收错误

            std::shared_ptr<RedisExchangeSharedState<Strategy>> m_state; ///< 共享状态
        };

        /**
         * @brief Redis 连接建立共享状态
         * @details 保存连接、认证和数据库选择过程中的所有中间状态
         */
        template<galay::utils::RingBufferBackendStrategy Strategy>
        struct RedisConnectSharedState
        {
            /**
             * @brief 连接阶段枚举
             */
            enum class Phase : uint8_t {
                Invalid, ///< 无效状态
                Connect, ///< 连接中
                Send,    ///< 发送中
                Parse,   ///< 解析中
                Done     ///< 完成
            };

            /**
             * @brief 待处理命令类型
             */
            enum class PendingCommand : uint8_t {
                None,   ///< 无
                Auth,   ///< AUTH 命令
                Select  ///< SELECT 命令
            };

            /**
             * @brief 构造连接共享状态
             */
            RedisConnectSharedState(RedisClient<Strategy>& client,
                                    std::string ip,
                                    int32_t port,
                                    std::string username,
                                    std::string password,
                                    int32_t db_index,
                                    int version);

            galay::kernel::Host host;                   ///< 主机地址
            std::string ip;                             ///< 服务器 IP
            std::string username;                       ///< 用户名
            std::string password;                       ///< 密码
            std::string encoded_cmd;                    ///< 已编码命令
            std::string parse_buffer;                   ///< 解析缓冲区
            std::vector<RedisValue> values;             ///< 解析得到的值
            std::array<struct iovec, 2> read_iovecs{};  ///< 读取 iovec 数组
            std::optional<RedisVoidResult> result;      ///< 连接结果
            RedisClient<Strategy>* client = nullptr;    ///< 关联的客户端
            size_t sent = 0;                            ///< 已发送字节数
            size_t read_iov_count = 0;                  ///< 读取 iovec 数量
            int32_t port = 0;                           ///< 服务器端口
            int32_t db_index = 0;                       ///< 数据库索引
            int version = 2;                            ///< RESP 协议版本
            Phase phase = Phase::Connect;               ///< 当前阶段
            PendingCommand pending_command = PendingCommand::None; ///< 待处理命令
            bool auth_sent = false;                     ///< AUTH 是否已发送
            bool select_sent = false;                   ///< SELECT 是否已发送
        };

        /**
         * @brief Redis 连接建立状态机
         * @details 驱动 TCP 连接、TLS 握手、认证和数据库选择的异步状态机
         */
        template<galay::utils::RingBufferBackendStrategy Strategy>
        struct RedisConnectMachine
        {
            using result_type = RedisVoidResult;
            static constexpr galay::kernel::SequenceOwnerDomain kSequenceOwnerDomain =
                galay::kernel::SequenceOwnerDomain::ReadWrite;

            /**
             * @brief 构造连接状态机
             * @param state 共享状态指针
             */
            explicit RedisConnectMachine(std::shared_ptr<RedisConnectSharedState<Strategy>> state);

            /**
             * @brief 推进状态机
             * @return 状态机动作
             */
            galay::kernel::MachineAction<result_type> advance();

            /**
             * @brief 连接完成回调
             * @param result 连接结果
             */
            void onConnect(std::expected<void, IOError> result);

            /**
             * @brief 读取完成回调
             * @param result 读取结果
             */
            void onRead(std::expected<size_t, IOError> result);

            /**
             * @brief 写入完成回调
             * @param result 写入结果
             */
            void onWrite(std::expected<size_t, IOError> result);

        private:
            bool prepareReadWindow();                                   ///< 准备读取窗口
            bool prepareNextCommand();                                  ///< 准备下一条命令
            std::expected<bool, RedisError> tryParseReply();            ///< 尝试解析回复
            void setError(RedisError error) noexcept;                  ///< 设置 Redis 错误
            void setConnectError(const IOError& io_error) noexcept;    ///< 设置连接错误
            void setSendError(const IOError& io_error) noexcept;       ///< 设置发送错误
            void setRecvError(const IOError& io_error) noexcept;       ///< 设置接收错误

            std::shared_ptr<RedisConnectSharedState<Strategy>> m_state; ///< 共享状态
        };

        template<galay::utils::RingBufferBackendStrategy Strategy>
        using RedisExchangeOperation =
            galay::kernel::StateMachineAwaitable<RedisExchangeMachine<Strategy>>;
        template<galay::utils::RingBufferBackendStrategy Strategy>
        using RedisConnectOperation =
            galay::kernel::StateMachineAwaitable<RedisConnectMachine<Strategy>>;
    } // namespace detail

    template<galay::utils::RingBufferBackendStrategy Strategy = galay::utils::RingBufferBackendStrategy::Mmap>
    using RedisExchangeOperationFor = detail::RedisExchangeOperation<Strategy>;
    template<galay::utils::RingBufferBackendStrategy Strategy = galay::utils::RingBufferBackendStrategy::Mmap>
    using RedisConnectOperationFor = detail::RedisConnectOperation<Strategy>;
    using RedisExchangeOperation = RedisExchangeOperationFor<>;
    using RedisConnectOperation = RedisConnectOperationFor<>;

#ifdef GALAY_SSL_FEATURE_ENABLED
    namespace detail
    {
        using RedissCommandResult =
            std::expected<std::optional<std::vector<RedisValue>>, RedisError>;

        struct RedissExchangeSharedState
        {
            enum class Phase : uint8_t {
                Invalid,
                Start,
                Send,
                Parse,
                Done
            };

            RedissExchangeSharedState(RedissClientImpl* impl,
                                      std::string encoded_command,
                                      size_t expected_replies,
                                      bool recv_only);

            std::string encoded_cmd;
            std::string parse_buffer;
            std::vector<RedisValue> values;
            std::array<struct iovec, 2> read_iovecs{};
            std::optional<RedissCommandResult> result;
            RedissClientImpl* impl = nullptr;
            char* read_buffer = nullptr;
            size_t expected_replies = 0;
            size_t sent = 0;
            size_t read_iov_count = 0;
            size_t read_length = 0;
            Phase phase = Phase::Start;
            bool recv_only = false;
        };

        struct RedissExchangeMachine
        {
            using result_type = RedissCommandResult;

            explicit RedissExchangeMachine(std::shared_ptr<RedissExchangeSharedState> state);

            galay::ssl::SslMachineAction<result_type> advance();
            void onHandshake(std::expected<void, galay::ssl::SslError> result);
            void onRecv(std::expected<galay::utils::Bytes, galay::ssl::SslError> result);
            void onSend(std::expected<size_t, galay::ssl::SslError> result);
            void onShutdown(std::expected<void, galay::ssl::SslError> result);

        private:
            bool prepareReadWindow();
            std::expected<bool, RedisError> tryParseReplies();
            void setError(RedisError error) noexcept;
            void setSendError(const galay::ssl::SslError& ssl_error) noexcept;
            void setRecvError(const galay::ssl::SslError& ssl_error) noexcept;

            std::shared_ptr<RedissExchangeSharedState> m_state;
        };

        struct RedissConnectSharedState
        {
            enum class Phase : uint8_t {
                Invalid,
                Connect,
                Handshake,
                Send,
                Parse,
                Done
            };

            enum class PendingCommand : uint8_t {
                None,
                Auth,
                Select
            };

            RedissConnectSharedState(RedissClientImpl* impl,
                                     std::string ip,
                                     int32_t port,
                                     RedisConnectOptions options);

            galay::kernel::Host host;
            RedisConnectOptions options;
            std::string ip;
            std::string encoded_cmd;
            std::string parse_buffer;
            std::vector<RedisValue> values;
            std::array<struct iovec, 2> read_iovecs{};
            std::optional<RedisVoidResult> result;
            RedissClientImpl* impl = nullptr;
            char* read_buffer = nullptr;
            size_t sent = 0;
            size_t read_iov_count = 0;
            size_t read_length = 0;
            int32_t port = 0;
            Phase phase = Phase::Connect;
            PendingCommand pending_command = PendingCommand::None;
            bool auth_sent = false;
            bool select_sent = false;
        };

        struct RedissConnectMachine
        {
            using result_type = RedisVoidResult;
            static constexpr galay::kernel::SequenceOwnerDomain kSequenceOwnerDomain =
                galay::kernel::SequenceOwnerDomain::ReadWrite;

            explicit RedissConnectMachine(std::shared_ptr<RedissConnectSharedState> state);

            galay::kernel::MachineAction<result_type> advance();
            void onConnect(std::expected<void, IOError> result);
            void onRead(std::expected<size_t, IOError> result);
            void onWrite(std::expected<size_t, IOError> result);

        private:
            bool prepareReadWindow();
            bool prepareNextCommand();
            std::expected<bool, RedisError> tryParseReply();
            galay::kernel::MachineAction<result_type> advanceSsl();
            void setError(RedisError error) noexcept;
            void setConnectError(const IOError& io_error) noexcept;
            void setSendError(const galay::ssl::SslError& ssl_error) noexcept;
            void setRecvError(const galay::ssl::SslError& ssl_error) noexcept;
            void handleHandshakeResult(std::expected<void, galay::ssl::SslError> result);
            void handleSendResult(std::expected<size_t, galay::ssl::SslError> result);
            void handleRecvResult(std::expected<galay::utils::Bytes, galay::ssl::SslError> result);

            std::shared_ptr<RedissConnectSharedState> m_state;
            galay::ssl::SslOperationDriver m_driver;
            bool m_ssl_active = false;
        };
        using RedissExchangeOperation =
            galay::ssl::SslStateMachineAwaitable<RedissExchangeMachine>;
        using RedissConnectOperation =
            galay::kernel::StateMachineAwaitable<RedissConnectMachine>;
    } // namespace detail
#endif

#endif // GALAY_REDIS_DETAILS_AWAITABLE_H
