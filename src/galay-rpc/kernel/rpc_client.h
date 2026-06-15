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
 * Coroutine callEcho(Runtime& runtime) {
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

#include "galay-rpc/common/rpc_log.h"
#include "rpc_conn.h"
#include "rpc_stream.h"
#include "galay-rpc/protoc/rpc_error.h"
#include "galay-rpc/protoc/rpc_message.h"
#include "galay-kernel/core/awaitable.h"
#include "galay-kernel/core/timeout.hpp"
#include <array>
#include <atomic>
#include <expected>
#include <memory>
#include <optional>

namespace galay::rpc
{

using namespace galay::kernel;

// 前向声明
template<typename SocketType>
class RpcClientImpl;

namespace detail {

/**
 * @brief 期望特定request_id的RPC响应读取状态
 *
 * @details 从RingBuffer中解析响应消息，并验证request_id与期望值匹配。
 */
class ExpectedRpcResponseReadState : public RpcRingBufferReadStateBase<RpcAwaitableResult>
{
public:
    ExpectedRpcResponseReadState(RingBuffer& ring_buffer,
                                 const RpcReaderSetting& setting,
                                 uint32_t expected_request_id,
                                 RpcResponse& response)
        : RpcRingBufferReadStateBase<RpcAwaitableResult>(ring_buffer)
        , m_setting(&setting)
        , m_expected_request_id(expected_request_id)
        , m_response(&response)
    {
    }

    /// @brief 从RingBuffer中尝试解析期望的响应消息
    bool parseFromRingBuffer()
    {
        if (ringBuffer().readable() == 0) {
            return false;
        }

        std::array<struct iovec, 2> read_iovecs{};
        const size_t read_iovecs_count = ringBuffer().getReadIovecs(read_iovecs);
        if (read_iovecs_count == 0) {
            return false;
        }

        const std::span<const iovec> read_span(read_iovecs.data(), read_iovecs_count);
        auto parse_result = tryParseResponseMessage(read_span,
                                                    iovecsReadableBytes(read_span),
                                                    m_setting->max_message_size,
                                                    *m_response);
        if (!parse_result.has_value()) {
            setReadError(parse_result.error());
            return true;
        }

        if (parse_result.value() == 0) {
            return false;
        }

        if (m_response->requestId() != m_expected_request_id) {
            setReadError(RpcError(RpcErrorCode::INVALID_RESPONSE,
                                  "Mismatched response request id"));
            return true;
        }

        ringBuffer().consume(parse_result.value());
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
template<typename SocketType>
class RecvRpcResponseChainAwaitable
    : public TimeoutSupport<RecvRpcResponseChainAwaitable<SocketType>> {
public:
    using Result = detail::RpcAwaitableResult;

    /**
     * @brief 构造响应接收等待体
     * @param ring_buffer 环形缓冲区
     * @param setting 读取配置
     * @param expected_request_id 期望匹配的请求ID
     * @param response 输出响应对象
     */
    RecvRpcResponseChainAwaitable(RingBuffer& ring_buffer,
                                  const RpcReaderSetting& setting,
                                  uint32_t expected_request_id,
                                  RpcResponse& response)
        : m_state(std::make_shared<detail::ExpectedRpcResponseReadState>(
            ring_buffer,
            setting,
            expected_request_id,
            response))
        , m_inner(
            AwaitableBuilder<Result>::fromStateMachine(
                nullptr,
                detail::RpcRingBufferReadMachine<detail::ExpectedRpcResponseReadState>(m_state))
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
        StateMachineAwaitable<detail::RpcRingBufferReadMachine<detail::ExpectedRpcResponseReadState>>;

    std::shared_ptr<detail::ExpectedRpcResponseReadState> m_state;  ///< 读取状态
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
    RpcReaderSetting reader_setting;            ///< 读取器配置
    RpcWriterSetting writer_setting;            ///< 写入器配置
    size_t ring_buffer_size = kDefaultRpcRingBufferSize;  ///< 环形缓冲区大小
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
    /// @brief 构建RpcClient实例
    RpcClientImpl<TcpSocket> build() const;
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
template<typename SocketType>
class RpcClientImpl {
public:
    /**
     * @brief 构造函数
     * @param config 客户端配置
     */
    explicit RpcClientImpl(const RpcClientConfig& config = RpcClientConfig())
        : m_socket(nullptr)
        , m_ring_buffer(nullptr)
        , m_config(config)
        , m_request_id(0)
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
     * @return 连接等待体
     */
    ConnectAwaitable connect(const std::string& host, uint16_t port) {
        RPC_LOG_INFO("[client] [connect]", "host={} port={}", host, port);
        m_socket = std::make_unique<SocketType>(IPType::IPV4);

        const size_t ring_buffer_size = m_config.ring_buffer_size == 0
            ? kDefaultRpcRingBufferSize
            : m_config.ring_buffer_size;
        m_ring_buffer = std::make_unique<RingBuffer>(ring_buffer_size);

        m_socket->option().handleNonBlock();

        Host server_host(IPType::IPV4, host, port);
        return m_socket->connect(server_host);
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
        if (!m_socket || !m_ring_buffer) {
            RPC_LOG_WARN("[client] [call] [not-connected]",
                         "service={} method={}",
                         service,
                         method);
            co_return RpcCallResult(std::unexpected(
                RpcError(RpcErrorCode::CONNECTION_CLOSED, "Client is not connected")));
        }

        uint32_t req_id = m_request_id.fetch_add(1, std::memory_order_relaxed);
        RpcRequest request(req_id, service, method);
        request.callMode(mode);
        request.endOfStream(end_of_stream);
        if (payload && payload_len > 0) {
            request.payload(payload, payload_len);
        }

        auto writer = getWriter();
        auto send_result = co_await writer.sendRequest(request);
        if (!send_result.has_value()) {
            RPC_LOG_WARN("[client] [send] [fail]",
                         "service={} method={} request_id={} code={} error={}",
                         service,
                         method,
                         req_id,
                         static_cast<int>(send_result.error().code()),
                         send_result.error().message());
            co_return RpcCallResult(std::unexpected(send_result.error()));
        }

        auto reader = getReader();
        RpcResponse response;
        auto recv_result = co_await reader.getResponse(response);
        if (!recv_result.has_value()) {
            RPC_LOG_WARN("[client] [recv] [fail]",
                         "service={} method={} request_id={} code={} error={}",
                         service,
                         method,
                         req_id,
                         static_cast<int>(recv_result.error().code()),
                         recv_result.error().message());
            co_return RpcCallResult(std::unexpected(recv_result.error()));
        }

        if (response.requestId() != request.requestId()) {
            RPC_LOG_ERROR("[client] [recv] [mismatch-id]",
                          "expected={} actual={}",
                          request.requestId(),
                          response.requestId());
            co_return RpcCallResult(std::unexpected(
                RpcError(RpcErrorCode::INVALID_RESPONSE, "Mismatched response request id")));
        }

        if (response.callMode() != request.callMode()) {
            RPC_LOG_ERROR("[client] [recv] [mismatch-mode]",
                          "expected={} actual={}",
                          static_cast<int>(request.callMode()),
                          static_cast<int>(response.callMode()));
            co_return RpcCallResult(std::unexpected(
                RpcError(RpcErrorCode::INVALID_RESPONSE, "Mismatched response call mode")));
        }

        co_return RpcCallResult(std::optional<RpcResponse>(std::move(response)));
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
     * @brief 调用远程方法（无payload）
     */
    RpcCallAwaitableImpl<SocketType> call(const std::string& service,
                                          const std::string& method) {
        return call(service, method, nullptr, 0);
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
     * @brief 创建流会话（自动分配 stream_id）
     *
     * @note 仅创建会话对象，不会自动执行 STREAM_INIT。
     */
    std::expected<RpcStreamImpl<SocketType>, RpcError> createStream(const std::string& service,
                                                                     const std::string& method) {
        const uint32_t stream_id = m_stream_id.fetch_add(1, std::memory_order_relaxed);
        return createStream(stream_id, service, method);
    }

    /**
     * @brief 创建流会话（显式指定 stream_id）
     *
     * @note 仅创建会话对象，不会自动执行 STREAM_INIT。
     */
    std::expected<RpcStreamImpl<SocketType>, RpcError> createStream(uint32_t stream_id,
                                                                     const std::string& service = {},
                                                                     const std::string& method = {}) {
        if (!m_socket || !m_ring_buffer) {
            RPC_LOG_WARN("[client] [stream] [not-connected]",
                         "stream_id={} service={} method={}",
                         stream_id,
                         service,
                         method);
            return std::unexpected(RpcError(RpcErrorCode::CONNECTION_CLOSED,
                                            "Client is not connected"));
        }
        return RpcStreamImpl<SocketType>(*m_socket, *m_ring_buffer, stream_id, service, method);
    }

    /**
     * @brief 关闭连接
     */
    CloseAwaitable close() {
        return m_socket->close();
    }

    /**
     * @brief 获取读取器
     */
    RpcReaderImpl<SocketType> getReader() {
        return RpcReaderImpl<SocketType>(*m_ring_buffer, m_config.reader_setting, *m_socket);
    }

    /**
     * @brief 获取写入器
     */
    RpcWriterImpl<SocketType> getWriter() {
        return RpcWriterImpl<SocketType>(m_config.writer_setting, *m_socket);
    }

    /**
     * @brief 获取底层socket
     */
    SocketType& socket() { return *m_socket; }

    /**
     * @brief 获取RingBuffer
     */
    RingBuffer& ringBuffer() { return *m_ring_buffer; }

    /**
     * @brief 获取读取配置
     */
    const RpcReaderSetting& readerSetting() const { return m_config.reader_setting; }

private:
    std::unique_ptr<SocketType> m_socket;         ///< 底层Socket
    std::unique_ptr<RingBuffer> m_ring_buffer;    ///< 环形缓冲区
    RpcClientConfig m_config;                     ///< 客户端配置
    std::atomic<uint32_t> m_request_id;           ///< 自增请求ID
    std::atomic<uint32_t> m_stream_id;            ///< 自增流ID
};

/// @brief RPC调用等待体类型别名（TcpSocket）
using RpcCallAwaitable = RpcCallAwaitableImpl<TcpSocket>;
/// @brief RPC客户端类型别名（TcpSocket）
using RpcClient = RpcClientImpl<TcpSocket>;
inline RpcClient RpcClientBuilder::build() const { return RpcClient(m_config); }

} // namespace galay::rpc

#endif // GALAY_RPC_CLIENT_H
