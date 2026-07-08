/**
 * @file rpc_client.h
 * @brief RPC客户端
 * @author galay-rpc
 * @version 1.0.0
 *
 * @details 提供RPC客户端功能，支持异步调用和超时控制。
 *
 * @example
 * @code
 * Task<void> callEcho(Runtime& runtime) {
 *     RpcClient client;
 *     auto connect_result = co_await client.connect("127.0.0.1", 9000);
 *     if (!connect_result) {
 *         co_return;
 *     }
 *
 *     auto result = co_await client.call("EchoService", "echo", "Hello").timeout(std::chrono::milliseconds(5000));
 *     if (result && result.value()) {
 *         auto& response = result.value().value();
 *         // 处理响应
 *     }
 *
 *     co_await client.close();
 * }
 * @endcode
 */

#ifndef GALAY_RPC_CLIENT_H
#define GALAY_RPC_CLIENT_H

#include "../common/rpc_log.h"
#include "rpc_channel.h"
#include "rpc_call.h"
#include "rpc_conn.h"
#include "rpc_reconnect.h"
#include "rpc_stream.h"
#include "../protoc/rpc_error.h"
#include "../protoc/rpc_message.h"
#include "../../galay-kernel/common/sleep.hpp"
#include "../../galay-kernel/core/awaitable.h"
#include "../../galay-kernel/core/timeout.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace galay::rpc
{

using namespace galay::kernel;
using ::galay::utils::RingBufferBackendStrategy;
using ::galay::utils::RingBuffer;

// 前向声明
template<typename SocketType, RingBufferBackendStrategy Strategy>
class RpcClientImpl;

namespace detail {

/**
 * @brief 期望特定request_id的RPC响应读取状态
 *
 * @details 从RingBuffer中解析响应消息，并验证request_id与期望值匹配。
 */
template<RingBufferBackendStrategy Strategy = RingBufferBackendStrategy::Mmap>
class ExpectedRpcResponseReadState : public RpcRingBufferReadStateBase<RpcAwaitableResult, Strategy>
{
public:
    using Base = RpcRingBufferReadStateBase<RpcAwaitableResult, Strategy>;

    ExpectedRpcResponseReadState(RingBuffer<Strategy>& ring_buffer,
                                 const RpcReaderSetting& setting,
                                 uint32_t expected_request_id,
                                 RpcResponse& response)
        : Base(ring_buffer)
        , m_setting(&setting)
        , m_expected_request_id(expected_request_id)
        , m_response(&response)
    {
    }

    /// @brief 从RingBuffer中尝试解析期望的响应消息
    bool parseFromRingBuffer()
    {
        if (this->ringBuffer().readable() == 0) {
            return false;
        }

        std::array<struct iovec, 2> read_iovecs{};
        const size_t read_iovecs_count = this->ringBuffer().getReadIovecs(read_iovecs);
        if (read_iovecs_count == 0) {
            return false;
        }

        const std::span<const iovec> read_span(read_iovecs.data(), read_iovecs_count);
        auto parse_result = tryParseResponseMessage(read_span,
                                                    iovecsReadableBytes(read_span),
                                                    m_setting->max_message_size,
                                                    *m_response);
        if (!parse_result.has_value()) {
            this->setReadError(parse_result.error());
            return true;
        }

        if (parse_result.value() == 0) {
            return false;
        }

        if (m_response->requestId() != m_expected_request_id) {
            this->setReadError(RpcError(RpcErrorCode::INVALID_RESPONSE,
                                        "Mismatched response request id"));
            return true;
        }

        this->ringBuffer().consume(parse_result.value());
        return true;
    }

private:
    const RpcReaderSetting* m_setting = nullptr;  ///< 读取配置
    uint32_t m_expected_request_id = 0;           ///< 期望的请求ID
    RpcResponse* m_response = nullptr;            ///< 输出响应对象
};

}  // namespace detail

/**
 * @brief 接收RPC响应的链式等待体
 *
 * @details 支持超时控制的RPC响应接收协程等待体，
 *          内部使用状态机驱动readv直到收到完整响应。
 * @tparam SocketType Socket类型
 */
template<typename SocketType, RingBufferBackendStrategy Strategy = RingBufferBackendStrategy::Mmap>
class RecvRpcResponseChainAwaitable
    : public TimeoutSupport<RecvRpcResponseChainAwaitable<SocketType, Strategy>> {
public:
    using Result = detail::RpcAwaitableResult;
    using ReadState = detail::ExpectedRpcResponseReadState<Strategy>;

    /**
     * @brief 构造响应接收等待体
     * @param ring_buffer 环形缓冲区
     * @param setting 读取配置
     * @param expected_request_id 期望匹配的请求ID
     * @param response 输出响应对象
     */
    RecvRpcResponseChainAwaitable(RingBuffer<Strategy>& ring_buffer,
                                  const RpcReaderSetting& setting,
                                  uint32_t expected_request_id,
                                  RpcResponse& response)
        : m_state(std::make_shared<ReadState>(
            ring_buffer,
            setting,
            expected_request_id,
            response))
        , m_inner(
            AwaitableBuilder<Result>::fromStateMachine(
                nullptr,
                detail::RpcRingBufferReadMachine<ReadState>(m_state))
                .build())
    {}

    RecvRpcResponseChainAwaitable(RecvRpcResponseChainAwaitable&&) noexcept = default;
    RecvRpcResponseChainAwaitable& operator=(RecvRpcResponseChainAwaitable&&) noexcept = default;
    RecvRpcResponseChainAwaitable(const RecvRpcResponseChainAwaitable&) = delete;
    RecvRpcResponseChainAwaitable& operator=(const RecvRpcResponseChainAwaitable&) = delete;

    bool await_ready() { return m_inner.await_ready(); }  ///< 检查是否已就绪
    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> handle)  ///< 挂起协程
    {
        return m_inner.await_suspend(handle);
    }
    Result await_resume() { return m_inner.await_resume(); }  ///< 恢复协程并返回结果
    void markTimeout() { m_inner.markTimeout(); }  ///< 标记超时

private:
    using InnerAwaitable =
        StateMachineAwaitable<detail::RpcRingBufferReadMachine<ReadState>>;

    std::shared_ptr<ReadState> m_state;  ///< 读取状态
    InnerAwaitable m_inner;  ///< 内部状态机等待体
};

/// @brief RPC调用结果类型
using RpcCallResult = std::expected<std::optional<RpcResponse>, RpcError>;

/// @brief RPC调用等待体实现类型
template<typename SocketType>
using RpcCallAwaitableImpl = Task<RpcCallResult>;

/**
 * @brief RPC客户端配置
 */
struct RpcClientConfig {
    bool tcp_no_delay = true;                   ///< 是否为连接 socket 启用 TCP_NODELAY
    RpcReaderSetting reader_setting;            ///< 读取器配置
    RpcWriterSetting writer_setting;            ///< 写入器配置
    size_t ring_buffer_size = kDefaultRpcRingBufferSize;  ///< 环形缓冲区大小
    RpcChannelOptions channel_options;          ///< 通道配置
};

/**
 * @brief RPC客户端构建器
 *
 * @details 使用Builder模式配置并创建RpcClient实例。
 */
class RpcClientBuilder {
public:
    /// @brief 设置读取器配置
    RpcClientBuilder& readerSetting(RpcReaderSetting setting) { m_config.reader_setting = std::move(setting); return *this; }
    /// @brief 设置写入器配置
    RpcClientBuilder& writerSetting(RpcWriterSetting setting) { m_config.writer_setting = std::move(setting); return *this; }
    /// @brief 设置环形缓冲区大小
    RpcClientBuilder& ringBufferSize(size_t size)             { m_config.ring_buffer_size = size; return *this; }
    /// @brief 设置连接 socket 是否启用 TCP_NODELAY
    RpcClientBuilder& tcpNoDelay(bool value)                   { m_config.tcp_no_delay = value; return *this; }
    /// @brief 设置metrics回调
    RpcClientBuilder& metricsCallback(RpcMetricCallback callback) { m_config.channel_options.metrics_callback = std::move(callback); return *this; }
    /// @brief 构建RpcClient实例
    RpcClientImpl<TcpSocket, RingBufferBackendStrategy::Mmap> build() const;
    /// @brief 仅导出配置
    RpcClientConfig buildConfig() const                       { return m_config; }

private:
    RpcClientConfig m_config;  ///< 客户端配置
};

/**
 * @brief RPC客户端模板类
 *
 * @details 提供RPC客户端的完整功能，包括连接、一元调用、流式调用和流会话管理。
 *          支持超时控制，使用协程进行异步IO操作。
 * @tparam SocketType 底层Socket类型，默认为TcpSocket
 */
template<typename SocketType, RingBufferBackendStrategy Strategy = RingBufferBackendStrategy::Mmap>
class RpcClientImpl {
public:
    /**
     * @brief 构造函数
     * @param config 客户端配置
     */
    explicit RpcClientImpl(const RpcClientConfig& config = RpcClientConfig())
        : m_channel(std::make_unique<RpcChannelImpl<SocketType, Strategy>>(
              config.reader_setting,
              config.writer_setting,
              config.ring_buffer_size,
              config.channel_options,
              config.tcp_no_delay))
        , m_config(config)
        , m_stream_id(1)
    {
    }

    ~RpcClientImpl() = default;

    // 禁止拷贝和移动
    RpcClientImpl(const RpcClientImpl&) = delete;
    RpcClientImpl& operator=(const RpcClientImpl&) = delete;
    RpcClientImpl(RpcClientImpl&&) = delete;
    RpcClientImpl& operator=(RpcClientImpl&&) = delete;

    /**
     * @brief 连接到服务器
     * @param host 服务器地址
     * @param port 服务器端口
     * @return 连接任务；成功返回void，失败返回最后一次IOError
     *
     * @details 启用重连策略时会在失败后通过协程sleep异步退避并重试，不阻塞OS线程。
     */
    Task<std::expected<void, IOError>> connect(const std::string& host, uint16_t port) {
        RPC_LOG_INFO("[client] [connect]", "host={} port={}", host, port);
        m_host = host;
        m_port = port;
        m_has_endpoint = true;
        auto result = co_await connectWithPolicy();
        if (!result.has_value()) {
            co_return std::unexpected(IOError(kConnectFailed, 0));
        }
        co_return std::move(result.value());
    }

    /**
     * @brief 设置客户端重连策略
     * @param policy 重连策略；max_attempts<=1表示保持默认不重试
     * @return 当前客户端，便于链式配置
     *
     * @note 该策略只影响后续connect和新call前的重连，不会自动重放已发送的pending调用。
     */
    RpcClientImpl& reconnectPolicy(RpcReconnectPolicy policy) {
        if (policy.max_attempts == 0) {
            policy.max_attempts = 1;
        }
        m_reconnect_policy = policy;
        return *this;
    }

    /**
     * @brief 调用远程方法
     * @param service 服务名
     * @param method 方法名
     * @param payload 请求数据
     * @param payload_len 数据长度
     * @return RPC调用等待体（支持超时）
     */
    RpcCallAwaitableImpl<SocketType> call(const std::string& service,
                                          const std::string& method,
                                          const char* payload,
                                          size_t payload_len) {
        return callWithMode(service, method, RpcCallMode::UNARY, true, payload, payload_len);
    }

    /**
     * @brief 按调用模式发送RPC帧（为流式RPC预留）
     *
     * @note 当前仍走一次请求对应一次响应链路；后续流式模式会复用该元信息扩展多帧流程。
     */
    RpcCallAwaitableImpl<SocketType> callWithMode(const std::string& service,
                                                  const std::string& method,
                                                  RpcCallMode mode,
                                                  bool end_of_stream,
                                                  const char* payload,
                                                  size_t payload_len) {
        return callWithModeOwned(std::string(service),
                                 std::string(method),
                                 mode,
                                 end_of_stream,
                                 copyPayload(payload, payload_len),
                                 RpcCallOptions{});
    }

    /**
     * @brief 按调用模式发送RPC帧并应用调用选项
     * @param options deadline、取消和metadata等调用级选项
     */
    RpcCallAwaitableImpl<SocketType> callWithMode(const std::string& service,
                                                  const std::string& method,
                                                  RpcCallMode mode,
                                                  bool end_of_stream,
                                                  const char* payload,
                                                  size_t payload_len,
                                                  const RpcCallOptions& options) {
        return callWithModeOwned(std::string(service),
                                 std::string(method),
                                 mode,
                                 end_of_stream,
                                 copyPayload(payload, payload_len),
                                 options);
    }

    /**
     * @brief 调用远程方法（字符串payload）
     */
    RpcCallAwaitableImpl<SocketType> call(const std::string& service,
                                          const std::string& method,
                                          const std::string& payload) {
        return call(service, method, payload.data(), payload.size());
    }

    /**
     * @brief 调用远程方法（字符串payload，带调用选项）
     */
    RpcCallAwaitableImpl<SocketType> call(const std::string& service,
                                          const std::string& method,
                                          const std::string& payload,
                                          const RpcCallOptions& options) {
        return callWithMode(service, method, RpcCallMode::UNARY, true, payload.data(), payload.size(), options);
    }

    /**
     * @brief 调用远程方法（buffer payload，带调用选项）
     */
    RpcCallAwaitableImpl<SocketType> call(const std::string& service,
                                          const std::string& method,
                                          const char* payload,
                                          size_t payload_len,
                                          const RpcCallOptions& options) {
        return callWithMode(service, method, RpcCallMode::UNARY, true, payload, payload_len, options);
    }

    /**
     * @brief 调用远程方法（无payload）
     */
    RpcCallAwaitableImpl<SocketType> call(const std::string& service,
                                          const std::string& method) {
        return call(service, method, nullptr, 0);
    }

    /**
     * @brief 调用远程方法（无payload，带调用选项）
     */
    RpcCallAwaitableImpl<SocketType> call(const std::string& service,
                                          const std::string& method,
                                          const RpcCallOptions& options) {
        return callWithMode(service, method, RpcCallMode::UNARY, true, nullptr, 0, options);
    }

    /**
     * @brief 客户端流帧发送（N frame -> 1 response）
     */
    RpcCallAwaitableImpl<SocketType> callClientStreamFrame(const std::string& service,
                                                           const std::string& method,
                                                           const char* payload,
                                                           size_t payload_len,
                                                           bool end_of_stream) {
        return callWithMode(service, method, RpcCallMode::CLIENT_STREAMING, end_of_stream, payload, payload_len);
    }

    /**
     * @brief 服务端流请求（1 request -> N response frame）
     */
    RpcCallAwaitableImpl<SocketType> callServerStreamRequest(const std::string& service,
                                                             const std::string& method,
                                                             const char* payload,
                                                             size_t payload_len) {
        return callWithMode(service, method, RpcCallMode::SERVER_STREAMING, true, payload, payload_len);
    }

    /**
     * @brief 双向流帧发送（N frame <-> N frame）
     */
    RpcCallAwaitableImpl<SocketType> callBidiStreamFrame(const std::string& service,
                                                         const std::string& method,
                                                         const char* payload,
                                                         size_t payload_len,
                                                         bool end_of_stream) {
        return callWithMode(service, method, RpcCallMode::BIDI_STREAMING, end_of_stream, payload, payload_len);
    }

    /**
     * @brief 发送一次HEARTBEAT并等待pong
     */
    Task<RpcHeartbeatResult> sendHeartbeat() {
        return m_channel->sendHeartbeat();
    }

    /**
     * @brief 创建流会话（自动分配 stream_id）
     *
     * @note 仅创建会话对象，不会自动执行 STREAM_INIT。
     */
    std::expected<RpcStreamImpl<SocketType, Strategy>, RpcError> createStream(const std::string& service,
                                                                               const std::string& method) {
        const uint32_t stream_id = m_stream_id.fetch_add(1, std::memory_order_relaxed);
        return createStream(stream_id, service, method);
    }

    /**
     * @brief 创建流会话（显式指定 stream_id）
     *
     * @note 仅创建会话对象，不会自动执行 STREAM_INIT。
     */
    std::expected<RpcStreamImpl<SocketType, Strategy>, RpcError> createStream(uint32_t stream_id,
                                                                               const std::string& service = {},
                                                                               const std::string& method = {}) {
        if (!m_channel || !m_connected || !m_channel->ready()) {
            RPC_LOG_WARN("[client] [stream] [not-connected]",
                         "stream_id={} service={} method={}",
                         stream_id,
                         service,
                         method);
            return std::unexpected(RpcError(RpcErrorCode::CONNECTION_CLOSED,
                                            "Client is not connected"));
        }
        return RpcStreamImpl<SocketType, Strategy>(m_channel->socket(), m_channel->ringBuffer(), stream_id, service, method);
    }

    /**
     * @brief 关闭连接
     */
    Task<std::expected<void, IOError>> close() {
        m_connected = false;
        if (!m_channel) {
            co_return std::expected<void, IOError>{};
        }
        auto result = co_await m_channel->close();
        if (!result.has_value()) {
            co_return std::unexpected(IOError(kDisconnectError, 0));
        }
        co_return std::move(result.value());
    }

    /**
     * @brief 获取读取器
     */
    RpcReaderImpl<SocketType, Strategy> getReader() {
        return m_channel->getReader();
    }

    /**
     * @brief 获取写入器
     */
    RpcWriterImpl<SocketType> getWriter() {
        return m_channel->getWriter();
    }

    /**
     * @brief 获取底层socket
     */
    SocketType& socket() { return m_channel->socket(); }

    /**
     * @brief 获取RingBuffer
     */
    RingBuffer<Strategy>& ringBuffer() { return m_channel->ringBuffer(); }

    /**
     * @brief 获取读取配置
     */
    const RpcReaderSetting& readerSetting() const { return m_config.reader_setting; }

private:
    static std::vector<char> copyPayload(const char* payload, size_t payload_len) {
        if (payload == nullptr || payload_len == 0) {
            return {};
        }
        return std::vector<char>(payload, payload + payload_len);
    }

    RpcCallAwaitableImpl<SocketType> callWithModeOwned(std::string service,
                                                       std::string method,
                                                       RpcCallMode mode,
                                                       bool end_of_stream,
                                                       std::vector<char> payload,
                                                       RpcCallOptions options) {
        if (auto reconnect_result = co_await ensureConnectedForNextCall(); !reconnect_result.has_value()) {
            co_return RpcCallResult(std::unexpected(
                RpcError(RpcErrorCode::UNAVAILABLE, "Failed to schedule RPC reconnect")));
        } else if (!reconnect_result.value().has_value()) {
            co_return RpcCallResult(std::unexpected(
                RpcError::from(reconnect_result.value().error(), RpcErrorCode::UNAVAILABLE)));
        }
        const char* payload_data = payload.empty() ? nullptr : payload.data();
        auto call_result = co_await m_channel->callWithMode(service,
                                                            method,
                                                            mode,
                                                            end_of_stream,
                                                            payload_data,
                                                            payload.size(),
                                                            options);
        if (!call_result.has_value()) {
            co_return RpcCallResult(std::unexpected(
                RpcError(RpcErrorCode::INTERNAL_ERROR, "Failed to schedule RPC call")));
        }
        markDisconnectedIfConnectionError(call_result.value());
        co_return std::move(call_result.value());
    }

    std::unique_ptr<RpcChannelImpl<SocketType, Strategy>> makeChannel() const {
        return std::make_unique<RpcChannelImpl<SocketType, Strategy>>(
            m_config.reader_setting,
            m_config.writer_setting,
            m_config.ring_buffer_size,
            m_config.channel_options,
            m_config.tcp_no_delay);
    }

    Task<std::expected<void, IOError>> connectWithPolicy() {
        std::expected<void, IOError> last_error = {};
        const size_t attempts = m_reconnect_policy.max_attempts == 0 ? 1 : m_reconnect_policy.max_attempts;
        for (size_t attempt = 0; attempt < attempts; ++attempt) {
            if (m_channel) {
                auto close_result = co_await m_channel->close();
                if (!close_result.has_value() || !close_result.value().has_value()) {
                    co_return std::unexpected(IOError(kDisconnectError, 0));
                }
            }
            auto channel = makeChannel();
            auto task_result = co_await channel->connect(m_host, m_port);
            if (!task_result.has_value()) {
                last_error = std::unexpected(IOError(kConnectFailed, 0));
                m_connected = false;
                if (attempt + 1 < attempts && m_reconnect_policy.backoff.count() > 0) {
                    co_await sleep(m_reconnect_policy.backoff);
                }
                continue;
            }

            auto result = std::move(task_result.value());
            if (result.has_value()) {
                m_channel = std::move(channel);
                m_connected = true;
                co_return std::move(result);
            }

            last_error = std::unexpected(result.error());
            m_connected = false;
            if (attempt + 1 < attempts && m_reconnect_policy.backoff.count() > 0) {
                co_await sleep(m_reconnect_policy.backoff);
            }
        }
        co_return last_error;
    }

    Task<std::expected<void, IOError>> ensureConnectedForNextCall() {
        if (m_connected) {
            co_return std::expected<void, IOError>{};
        }
        if (!m_has_endpoint || m_reconnect_policy.max_attempts <= 1) {
            co_return std::unexpected(IOError(kConnectFailed, 0));
        }
        auto result = co_await connectWithPolicy();
        if (!result.has_value()) {
            co_return std::unexpected(IOError(kConnectFailed, 0));
        }
        co_return std::move(result.value());
    }

    void markDisconnectedIfConnectionError(const RpcCallResult& result) {
        if (result.has_value()) {
            return;
        }
        const auto code = result.error().code();
        if (code == RpcErrorCode::CONNECTION_CLOSED || code == RpcErrorCode::UNAVAILABLE) {
            m_connected = false;
        }
    }

    std::unique_ptr<RpcChannelImpl<SocketType, Strategy>> m_channel;  ///< 一元RPC连接通道
    RpcClientConfig m_config;                     ///< 客户端配置
    std::atomic<uint32_t> m_stream_id;            ///< 自增流ID
    RpcReconnectPolicy m_reconnect_policy;        ///< opt-in重连策略
    std::string m_host;                            ///< 最近一次连接地址
    uint16_t m_port = 0;                           ///< 最近一次连接端口
    bool m_has_endpoint = false;                   ///< 是否已有可重连端点
    bool m_connected = false;                      ///< 最近一次连接是否成功
};

/// @brief RPC调用等待体类型别名（TcpSocket）
using RpcCallAwaitable = RpcCallAwaitableImpl<TcpSocket>;
/// @brief RPC客户端类型别名（TcpSocket）
using RpcClient = RpcClientImpl<TcpSocket>;
inline RpcClient RpcClientBuilder::build() const { return RpcClient(m_config); }

} // namespace galay::rpc

#endif // GALAY_RPC_CLIENT_H
