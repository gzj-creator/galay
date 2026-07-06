/**
 * @file redis_client.h
 * @brief Redis 异步客户端核心实现
 * @author galay-redis
 * @version 1.0.0
 *
 * @details 提供 Redis/Rediss 异步客户端、构建器、命令交换状态机和连接状态机，
 *          基于 galay-kernel 的协程和 IO 调度框架实现高性能异步 Redis 通信。
 */

#ifndef GALAY_REDIS_CLIENT_H
#define GALAY_REDIS_CLIENT_H

#include "../../galay-kernel/async/tcp_socket.h"
#include "../../galay-kernel/core/io_scheduler.hpp"
#include "../../galay-kernel/core/task.h"
#include "../../galay-kernel/core/timeout.hpp"
#include "../../galay-kernel/common/host.hpp"
#include "../../galay-kernel/common/error.h"
#include "../../galay-utils/cache/ring_buffer.hpp"
#ifdef GALAY_SSL_FEATURE_ENABLED
#include "../../galay-utils/cache/bytes.hpp"
#include "../../galay-ssl/async/ssl_await.h"
#include "../../galay-ssl/async/ssl_socket.h"
#endif
#include <concepts>
#include <memory>
#include <string>
#include <expected>
#include <optional>
#include <vector>
#include <coroutine>
#include <utility>
#include <span>
#include <array>
#include <string_view>
#include <sys/uio.h>
#include "../base/redis_error.h"
#include "../base/redis_log.h"
#include "../base/redis_value.h"
#include "../protoc/redis_protocol.h"
#include "../protoc/builder.h"
#include "../base/redis_config.h"

namespace galay::redis
{
    using galay::async::TcpSocket;
    using galay::kernel::IOScheduler;
    using galay::kernel::Host;
    using galay::kernel::IOError;
    using galay::kernel::IPType;
    using galay::kernel::Task;
    using galay::kernel::TaskRef;


    // 类型别名
    using RedisResult = std::expected<std::vector<RedisValue>, RedisError>; ///< Redis 命令结果类型
    using RedisVoidResult = std::expected<void, RedisError>;               ///< Redis 无返回值结果类型

    /**
     * @brief 零拷贝借用命令包
     * @details 内部快速路径数据包，用于可信调用者的零拷贝发送。
     *          encoded 字符串必须在整个 co_await 期间保持有效，禁止传递临时对象。
     */
    class RedisBorrowedCommand
    {
    public:
        /**
         * @brief 从 const 字符串引用构造借用命令
         * @param encoded 已编码的 RESP 命令字符串
         * @param expected_replies 期望的回复数量
         */
        explicit RedisBorrowedCommand(const std::string& encoded,
                                      size_t expected_replies = 1) noexcept
            : m_encoded(encoded)
            , m_expected_replies(expected_replies)
        {
        }

        RedisBorrowedCommand(std::string&&, size_t = 1) = delete;       ///< 禁止右值构造
        RedisBorrowedCommand(std::string_view, size_t = 1) = delete;    ///< 禁止 string_view 构造

        [[nodiscard]] std::string_view encoded() const noexcept { return m_encoded; }          ///< 获取编码后的命令视图
        [[nodiscard]] size_t expectedReplies() const noexcept { return m_expected_replies; }  ///< 获取期望回复数量

    private:
        std::string_view m_encoded;          ///< 编码后的命令视图
        size_t m_expected_replies = 1;       ///< 期望的回复数量
    };

    /**
     * @brief Redis 连接选项
     * @details 包含认证和数据库选择等连接参数
     */
    struct RedisConnectOptions
    {
        std::string username;     ///< 认证用户名
        std::string password;     ///< 认证密码
        int32_t db_index = 0;     ///< 数据库索引
        int version = 2;          ///< RESP 协议版本（2 或 3）
    };

#ifdef GALAY_SSL_FEATURE_ENABLED
    /**
     * @brief Rediss（TLS）客户端配置
     * @details 包含 TLS 证书验证相关参数
     */
    struct RedissClientConfig
    {
        std::string ca_path;       ///< CA 证书路径
        std::string server_name;   ///< SNI 服务器名称
        int verify_depth = 4;      ///< 证书链验证深度
        bool verify_peer = false;  ///< 是否验证对端证书
    };
#endif

    // 前向声明
    class RedisClient;
#ifdef GALAY_SSL_FEATURE_ENABLED
    class RedissClient;

    namespace detail
    {
        struct RedissClientImpl;
    }
#endif

    /**
     * @brief Redis 客户端构建器
     * @details 使用建造者模式逐步配置并构建 RedisClient 实例
     */
    class RedisClientBuilder
    {
    public:
        /**
         * @brief 设置 IO 调度器
         * @param scheduler IO 调度器指针
         * @return 构建器引用，支持链式调用
         */
        RedisClientBuilder& scheduler(IOScheduler* scheduler)
        {
            m_scheduler = scheduler;
            return *this;
        }

        /**
         * @brief 设置异步配置
         * @param config 异步 Redis 配置
         * @return 构建器引用
         */
        RedisClientBuilder& config(AsyncRedisConfig config)
        {
            m_config = std::move(config);
            return *this;
        }

        /**
         * @brief 设置发送超时
         * @param timeout 发送超时时间
         * @return 构建器引用
         */
        RedisClientBuilder& sendTimeout(std::chrono::milliseconds timeout)
        {
            m_config.send_timeout = timeout;
            return *this;
        }

        /**
         * @brief 设置接收超时
         * @param timeout 接收超时时间
         * @return 构建器引用
         */
        RedisClientBuilder& recvTimeout(std::chrono::milliseconds timeout)
        {
            m_config.recv_timeout = timeout;
            return *this;
        }

        /**
         * @brief 设置缓冲区大小
         * @param size 缓冲区大小（字节）
         * @return 构建器引用
         */
        RedisClientBuilder& bufferSize(size_t size)
        {
            m_config.buffer_size = size;
            return *this;
        }

        /**
         * @brief 设置连接 socket 是否启用 TCP_NODELAY
         * @param enabled 是否启用
         * @return 构建器引用
         */
        RedisClientBuilder& tcpNoDelay(bool enabled)
        {
            m_config.tcp_no_delay = enabled;
            return *this;
        }

        /**
         * @brief 构建 RedisClient 实例
         * @return 配置完成的 RedisClient
         */
        RedisClient build() const;

        /**
         * @brief 获取当前构建的配置
         * @return 异步 Redis 配置
         */
        AsyncRedisConfig buildConfig() const
        {
            return m_config;
        }

    private:
        IOScheduler* m_scheduler = nullptr;                                     ///< IO 调度器
        AsyncRedisConfig m_config = AsyncRedisConfig::noTimeout();              ///< 异步配置
    };

#ifdef GALAY_SSL_FEATURE_ENABLED
    /**
     * @brief Rediss（TLS）客户端构建器
     * @details 使用建造者模式逐步配置并构建 RedissClient 实例，
     *          在 RedisClientBuilder 的基础上增加了 TLS 相关配置
     */
    class RedissClientBuilder
    {
    public:
        /**
         * @brief 设置 IO 调度器
         * @param scheduler IO 调度器指针
         * @return 构建器引用
         */
        RedissClientBuilder& scheduler(IOScheduler* scheduler)
        {
            m_scheduler = scheduler;
            return *this;
        }

        /**
         * @brief 设置异步配置
         * @param config 异步 Redis 配置
         * @return 构建器引用
         */
        RedissClientBuilder& config(AsyncRedisConfig config)
        {
            m_config = std::move(config);
            return *this;
        }

        /**
         * @brief 设置 TLS 配置
         * @param config Rediss TLS 配置
         * @return 构建器引用
         */
        RedissClientBuilder& tlsConfig(RedissClientConfig config)
        {
            m_tls_config = std::move(config);
            return *this;
        }

        /**
         * @brief 设置发送超时
         * @param timeout 发送超时时间
         * @return 构建器引用
         */
        RedissClientBuilder& sendTimeout(std::chrono::milliseconds timeout)
        {
            m_config.send_timeout = timeout;
            return *this;
        }

        /**
         * @brief 设置接收超时
         * @param timeout 接收超时时间
         * @return 构建器引用
         */
        RedissClientBuilder& recvTimeout(std::chrono::milliseconds timeout)
        {
            m_config.recv_timeout = timeout;
            return *this;
        }

        /**
         * @brief 设置缓冲区大小
         * @param size 缓冲区大小（字节）
         * @return 构建器引用
         */
        RedissClientBuilder& bufferSize(size_t size)
        {
            m_config.buffer_size = size;
            return *this;
        }

        /**
         * @brief 设置底层 TCP socket 是否启用 TCP_NODELAY
         * @param enabled 是否启用
         * @return 构建器引用
         */
        RedissClientBuilder& tcpNoDelay(bool enabled)
        {
            m_config.tcp_no_delay = enabled;
            return *this;
        }

        /**
         * @brief 设置 CA 证书路径
         * @param path CA 证书文件路径
         * @return 构建器引用
         */
        RedissClientBuilder& caPath(std::string path)
        {
            m_tls_config.ca_path = std::move(path);
            return *this;
        }

        /**
         * @brief 设置是否验证对端证书
         * @param verify_peer 是否验证
         * @return 构建器引用
         */
        RedissClientBuilder& verifyPeer(bool verify_peer)
        {
            m_tls_config.verify_peer = verify_peer;
            return *this;
        }

        /**
         * @brief 设置证书链验证深度
         * @param verify_depth 验证深度
         * @return 构建器引用
         */
        RedissClientBuilder& verifyDepth(int verify_depth)
        {
            m_tls_config.verify_depth = verify_depth;
            return *this;
        }

        /**
         * @brief 设置 SNI 服务器名称
         * @param server_name 服务器名称
         * @return 构建器引用
         */
        RedissClientBuilder& serverName(std::string server_name)
        {
            m_tls_config.server_name = std::move(server_name);
            return *this;
        }

        /**
         * @brief 构建 RedissClient 实例
         * @return 配置完成的 RedissClient
         */
        RedissClient build() const;

        /**
         * @brief 获取当前构建的配置
         * @return 异步 Redis 配置
         */
        AsyncRedisConfig buildConfig() const
        {
            return m_config;
        }

        /**
         * @brief 获取当前构建的 TLS 配置
         * @return Rediss TLS 配置
         */
        RedissClientConfig buildTlsConfig() const
        {
            return m_tls_config;
        }

    private:
        IOScheduler* m_scheduler = nullptr;                                     ///< IO 调度器
        AsyncRedisConfig m_config = AsyncRedisConfig::noTimeout();              ///< 异步配置
        RedissClientConfig m_tls_config;                                        ///< TLS 配置
    };
#endif

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
            RedisExchangeSharedState(RedisClient& client,
                                     std::string encoded_command,
                                     size_t expected_replies,
                                     bool recv_only);
            /**
             * @brief 从字符串视图构造（零拷贝）
             */
            RedisExchangeSharedState(RedisClient& client,
                                     std::string_view encoded_command,
                                     size_t expected_replies,
                                     bool recv_only);
            /**
             * @brief 从批量命令视图构造
             */
            RedisExchangeSharedState(RedisClient& client,
                                     std::span<const RedisCommandView> commands);

            std::string encoded_cmd;                    ///< 已编码的命令字符串
            std::string parse_buffer;                   ///< 解析缓冲区
            std::vector<RedisValue> values;             ///< 解析得到的值
            std::array<struct iovec, 2> read_iovecs{};  ///< 读取 iovec 数组
            std::optional<RedisExchangeResult> result;  ///< 交换结果
            std::string_view encoded_view;              ///< 已编码的命令视图
            RedisClient* client = nullptr;              ///< 关联的客户端
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
        struct RedisExchangeMachine
        {
            using result_type = RedisExchangeResult;
            static constexpr galay::kernel::SequenceOwnerDomain kSequenceOwnerDomain =
                galay::kernel::SequenceOwnerDomain::ReadWrite;

            /**
             * @brief 构造交换状态机
             * @param state 共享状态指针
             */
            explicit RedisExchangeMachine(std::shared_ptr<RedisExchangeSharedState> state);

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

            std::shared_ptr<RedisExchangeSharedState> m_state;         ///< 共享状态
        };

        /**
         * @brief Redis 连接建立共享状态
         * @details 保存连接、认证和数据库选择过程中的所有中间状态
         */
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
            RedisConnectSharedState(RedisClient& client,
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
            RedisClient* client = nullptr;              ///< 关联的客户端
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
        struct RedisConnectMachine
        {
            using result_type = RedisVoidResult;
            static constexpr galay::kernel::SequenceOwnerDomain kSequenceOwnerDomain =
                galay::kernel::SequenceOwnerDomain::ReadWrite;

            /**
             * @brief 构造连接状态机
             * @param state 共享状态指针
             */
            explicit RedisConnectMachine(std::shared_ptr<RedisConnectSharedState> state);

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

            std::shared_ptr<RedisConnectSharedState> m_state;          ///< 共享状态
        };

        using RedisExchangeOperation =
            galay::kernel::StateMachineAwaitable<RedisExchangeMachine>;
        using RedisConnectOperation =
            galay::kernel::StateMachineAwaitable<RedisConnectMachine>;
    } // namespace detail

    using RedisExchangeOperation = detail::RedisExchangeOperation;
    using RedisConnectOperation = detail::RedisConnectOperation;

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

    /**
     * @brief Redis客户端类
     * @details 提供异步Redis客户端功能，采用Awaitable模式
     */
    class RedisClient
    {
    public:
        RedisClient(IOScheduler* scheduler,
                    AsyncRedisConfig config = AsyncRedisConfig::noTimeout());

        /**
         * @brief 移动构造函数
         * @warning 不要在操作进行中移动 RedisClient
         * @warning 确保所有 awaitable 都处于 Invalid 状态
         */
        RedisClient(RedisClient&& other) noexcept;

        /**
         * @brief 移动赋值运算符
         * @warning 不要在操作进行中移动 RedisClient
         * @warning 确保所有 awaitable 都处于 Invalid 状态
         */
        RedisClient& operator=(RedisClient&& other) noexcept;

        // 禁止拷贝
        RedisClient(const RedisClient&) = delete;
        RedisClient& operator=(const RedisClient&) = delete;

        // ======================== 连接方法 ========================

        /**
         * @brief 通过 URL 连接到 Redis 服务器
         * @param url Redis 连接 URL
         * @return RedisConnectOperation 连接操作
         */
        RedisConnectOperation connect(const std::string& url);

        /**
         * @brief 连接到指定地址的 Redis 服务器
         * @param ip 服务器 IP 地址
         * @param port 服务器端口
         * @param options 连接选项（认证、数据库等）
         * @return RedisConnectOperation 连接操作
         */
        RedisConnectOperation connect(const std::string& ip,
                                      int32_t port,
                                      RedisConnectOptions options = {});

        // ======================== 命令执行 ========================

        /**
         * @brief 执行单条 Redis 命令
         * @param command_packet 已编码的命令包
         * @return 命令交换操作等待体
         */
        RedisExchangeOperation command(RedisEncodedCommand command_packet);

        /**
         * @brief 零拷贝执行单条 Redis 命令
         * @param packet 借用命令包，必须在整个 co_await 期间保持有效
         * @return 命令交换操作等待体
         */
        RedisExchangeOperation commandBorrowed(const RedisBorrowedCommand& packet);
        RedisExchangeOperation commandBorrowed(RedisBorrowedCommand&& packet) = delete; ///< 禁止右值

        /**
         * @brief 仅接收指定数量的回复（不发送命令）
         * @param expected_replies 期望的回复数量
         * @return 命令交换操作等待体
         */
        RedisExchangeOperation receive(size_t expected_replies = 1);

        // ======================== Pipeline批量操作 ========================

        /**
         * @brief 批量执行多条 Redis 命令（Pipeline）
         * @param commands 命令视图数组
         * @return 命令交换操作等待体
         */
        RedisExchangeOperation batch(std::span<const RedisCommandView> commands);

        /**
         * @brief 零拷贝批量执行预编码的 Pipeline
         * @param encoded 已编码的 Pipeline 数据
         * @param expected_replies 期望的回复数量
         * @return 命令交换操作等待体
         */
        RedisExchangeOperation batchBorrowed(const std::string& encoded, size_t expected_replies);
        RedisExchangeOperation batchBorrowed(std::string&& encoded, size_t expected_replies) = delete; ///< 禁止右值

        // ======================== 连接管理 ========================

        TcpSocket& socket() { return m_socket; }                               ///< 获取底层 TCP 套接字
        protocol::RespParser& parser() { return m_parser; }                    ///< 获取 RESP 解析器
        galay::utils::RingBuffer& ringBuffer() { return *m_ring_buffer; }      ///< 获取接收环形缓冲区
        const galay::utils::RingBuffer& ringBuffer() const { return *m_ring_buffer; } ///< 获取接收环形缓冲区
        const AsyncRedisConfig& asyncConfig() const { return m_config; }       ///< 获取异步配置
        void setClosed(bool closed) { m_is_closed = closed; }                  ///< 设置关闭状态

        /**
         * @brief 关闭连接
         * @return 关闭操作等待体
         */
        auto close() {
            return m_socket.close();
        }

        bool isClosed() const { return m_is_closed; } ///< 检查连接是否已关闭

        ~RedisClient() = default;

    private:
        // 成员变量
        TcpSocket m_socket;                                   ///< TCP 套接字
        AsyncRedisConfig m_config;                            ///< 异步配置
        std::shared_ptr<galay::utils::RingBuffer> m_ring_buffer; ///< 接收环形缓冲区
        IOScheduler* m_scheduler;                             ///< IO 调度器
        bool m_is_closed = false;                             ///< 连接关闭标志
        protocol::RespParser m_parser;                        ///< RESP 协议解析器
    };

#ifdef GALAY_SSL_FEATURE_ENABLED
    /**
     * @brief Rediss（TLS）客户端类
     * @details 提供基于 TLS 加密的异步 Redis 客户端功能，采用 Pimpl 模式隐藏实现细节
     */
    class RedissClient
    {
    public:
        /**
         * @brief 构造 RedissClient
         * @param scheduler IO 调度器
         * @param config 异步 Redis 配置
         * @param tls_config TLS 配置
         */
        RedissClient(IOScheduler* scheduler,
                     AsyncRedisConfig config = AsyncRedisConfig::noTimeout(),
                     RedissClientConfig tls_config = {});
        RedissClient(RedissClient&& other) noexcept;                          ///< 移动构造
        RedissClient& operator=(RedissClient&& other) noexcept;               ///< 移动赋值
        RedissClient(const RedissClient&) = delete;                           ///< 禁止拷贝
        RedissClient& operator=(const RedissClient&) = delete;                ///< 禁止拷贝赋值
        ~RedissClient();

        /**
         * @brief 通过 URL 连接到 Redis 服务器
         * @param url Redis 连接 URL
         * @return 连接操作等待体
         */
        detail::RedissConnectOperation connect(const std::string& url);

        /**
         * @brief 连接到指定地址的 Redis 服务器
         * @param ip 服务器 IP 地址
         * @param port 服务器端口
         * @param options 连接选项
         * @return 连接操作等待体
         */
        detail::RedissConnectOperation connect(const std::string& ip,
                                              int32_t port,
                                              RedisConnectOptions options = {});

        /**
         * @brief 执行单条 Redis 命令
         * @param command_packet 已编码的命令
         * @return 命令交换操作等待体
         */
        detail::RedissExchangeOperation command(RedisEncodedCommand command_packet);

        /**
         * @brief 仅接收指定数量的回复
         * @param expected_replies 期望的回复数量
         * @return 命令交换操作等待体
         */
        detail::RedissExchangeOperation receive(size_t expected_replies = 1);

        /**
         * @brief 批量执行多条 Redis 命令
         * @param commands 命令视图数组
         * @return 命令交换操作等待体
         */
        detail::RedissExchangeOperation batch(std::span<const RedisCommandView> commands);

        const AsyncRedisConfig& asyncConfig() const;                          ///< 获取异步配置
        const RedissClientConfig& tlsConfig() const;                          ///< 获取 TLS 配置
        bool isClosed() const;                                                ///< 检查连接是否已关闭
        void setClosed(bool closed);                                          ///< 设置关闭状态
        galay::kernel::CloseAwaitable close();                                ///< 关闭连接

    private:
        std::unique_ptr<detail::RedissClientImpl> m_impl; ///< Pimpl 实现指针
    };
#endif

    inline galay::redis::RedisClient galay::redis::RedisClientBuilder::build() const
    {
        return RedisClient(m_scheduler, m_config);
    }

#ifdef GALAY_SSL_FEATURE_ENABLED
    inline galay::redis::RedissClient galay::redis::RedissClientBuilder::build() const
    {
        return RedissClient(m_scheduler, m_config, m_tls_config);
    }
#endif

}

#endif // GALAY_REDIS_CLIENT_H
