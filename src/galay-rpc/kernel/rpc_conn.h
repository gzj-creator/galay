/**
 * @file rpc_conn.h
 * @brief RPC连接封装
 * @author galay-rpc
 * @version 1.0.0
 *
 * @details 封装TCP连接，使用RingBuffer配合readv/writev提供高效的RPC消息读写。
 */

#ifndef GALAY_RPC_CONN_H
#define GALAY_RPC_CONN_H

#include "rpc_await.h"
#include "galay-rpc/protoc/rpc_message.h"
#include "galay-rpc/protoc/rpc_error.h"
#include "galay-kernel/async/tcp_socket.h"
#include "galay-kernel/core/awaitable.h"
#include "galay-kernel/core/task.h"
#include <galay-utils/cache/ring_buffer.hpp>
#include <algorithm>
#include <array>
#include <cstring>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace galay::rpc
{

using namespace galay::kernel;
using namespace galay::async;
using ::galay::utils::RingBuffer;

using Coroutine = Task<void>;

// 前向声明
template<typename SocketType> class RpcConnImpl;
template<typename SocketType> class RpcReaderImpl;
template<typename SocketType> class RpcWriterImpl;

/// @brief RPC连接默认环形缓冲区大小（8KB）
inline constexpr size_t kDefaultRpcRingBufferSize = 8 * 1024;

namespace detail {

/// @brief 计算iovec数组中可读取的总字节数
inline size_t iovecsReadableBytes(std::span<const iovec> iovecs) {
    size_t total = 0;
    for (const auto& iov : iovecs) {
        total += iov.iov_len;
    }
    return total;
}

/**
 * @brief 从iovec数组中拷贝指定偏移和长度的数据到输出缓冲区
 * @param iovecs iovec数组
 * @param src_offset 源偏移量
 * @param out 输出缓冲区
 * @param bytes 要拷贝的字节数
 * @return 是否成功拷贝了请求的字节数
 */
inline bool copyFromIovecs(std::span<const iovec> iovecs,
                           size_t src_offset,
                           char* out,
                           size_t bytes) {
    size_t copied = 0;
    size_t offset = src_offset;

    for (const auto& iov : iovecs) {
        if (copied >= bytes) {
            break;
        }

        if (offset >= iov.iov_len) {
            offset -= iov.iov_len;
            continue;
        }

        const auto* base = static_cast<const char*>(iov.iov_base);
        const size_t available = iov.iov_len - offset;
        const size_t to_copy = std::min(available, bytes - copied);
        std::memcpy(out + copied, base + offset, to_copy);
        copied += to_copy;
        offset = 0;
    }

    return copied == bytes;
}

/**
 * @brief 从iovec数组中构建payload视图（零拷贝）
 * @param iovecs iovec数组
 * @param src_offset 源偏移量
 * @param bytes 要提取的字节数
 * @param view 输出的payload视图
 * @return 是否成功构建（最多支持两段视图）
 */
inline bool payloadViewFromIovecs(std::span<const iovec> iovecs,
                                  size_t src_offset,
                                  size_t bytes,
                                  RpcPayloadView& view) {
    view = RpcPayloadView{};
    if (bytes == 0) {
        return true;
    }

    size_t remaining = bytes;
    size_t offset = src_offset;

    for (const auto& iov : iovecs) {
        if (remaining == 0) {
            break;
        }

        if (offset >= iov.iov_len) {
            offset -= iov.iov_len;
            continue;
        }

        const auto* base = static_cast<const char*>(iov.iov_base);
        const size_t available = iov.iov_len - offset;
        const size_t take = std::min(available, remaining);
        const char* segment_ptr = base + offset;

        if (view.segment1_len == 0) {
            view.segment1 = segment_ptr;
            view.segment1_len = take;
        } else if (view.segment2_len == 0) {
            view.segment2 = segment_ptr;
            view.segment2_len = take;
        } else {
            return false;
        }

        remaining -= take;
        offset = 0;
    }

    return remaining == 0;
}

/**
 * @brief 尝试从iovec数组中解析RPC请求消息
 * @param iovecs iovec数组
 * @param total_readable 可读取的总字节数
 * @param max_message_size 最大消息体大小
 * @param request 输出的请求对象
 * @return 解析消耗的字节数（0表示数据不足），或错误
 */
inline std::expected<size_t, RpcError> tryParseRequestMessage(std::span<const iovec> iovecs,
                                                              size_t total_readable,
                                                              size_t max_message_size,
                                                              RpcRequest& request) {
    if (total_readable < RPC_HEADER_SIZE) {
        return 0;
    }

    char header_buf[RPC_HEADER_SIZE];
    if (!copyFromIovecs(iovecs, 0, header_buf, RPC_HEADER_SIZE)) {
        return std::unexpected(RpcError(RpcErrorCode::INVALID_REQUEST, "Failed to read request header"));
    }

    RpcHeader header;
    if (!header.deserialize(header_buf)) {
        return std::unexpected(RpcError(RpcErrorCode::INVALID_REQUEST, "Invalid message header"));
    }

    if (header.m_type != static_cast<uint8_t>(RpcMessageType::REQUEST)) {
        return std::unexpected(RpcError(RpcErrorCode::INVALID_REQUEST, "Not a request message"));
    }

    const size_t msg_len = RPC_HEADER_SIZE + header.m_body_length;
    if (msg_len > max_message_size + RPC_HEADER_SIZE) {
        return std::unexpected(RpcError(RpcErrorCode::INVALID_REQUEST, "Message too large"));
    }

    if (total_readable < msg_len) {
        return 0;
    }

    size_t cursor = RPC_HEADER_SIZE;
    uint16_t service_len_net = 0;
    if (!copyFromIovecs(iovecs, cursor, reinterpret_cast<char*>(&service_len_net), sizeof(service_len_net))) {
        return std::unexpected(RpcError(RpcErrorCode::DESERIALIZATION_ERROR, "Invalid service length"));
    }
    const size_t service_len = rpcNtohs(service_len_net);
    cursor += sizeof(service_len_net);

    if (cursor + service_len > msg_len) {
        return std::unexpected(RpcError(RpcErrorCode::DESERIALIZATION_ERROR, "Service field out of range"));
    }
    std::string service_name(service_len, '\0');
    if (service_len > 0 && !copyFromIovecs(iovecs, cursor, service_name.data(), service_len)) {
        return std::unexpected(RpcError(RpcErrorCode::DESERIALIZATION_ERROR, "Invalid service field"));
    }
    cursor += service_len;

    uint16_t method_len_net = 0;
    if (!copyFromIovecs(iovecs, cursor, reinterpret_cast<char*>(&method_len_net), sizeof(method_len_net))) {
        return std::unexpected(RpcError(RpcErrorCode::DESERIALIZATION_ERROR, "Invalid method length"));
    }
    const size_t method_len = rpcNtohs(method_len_net);
    cursor += sizeof(method_len_net);

    if (cursor + method_len > msg_len) {
        return std::unexpected(RpcError(RpcErrorCode::DESERIALIZATION_ERROR, "Method field out of range"));
    }
    std::string method_name(method_len, '\0');
    if (method_len > 0 && !copyFromIovecs(iovecs, cursor, method_name.data(), method_len)) {
        return std::unexpected(RpcError(RpcErrorCode::DESERIALIZATION_ERROR, "Invalid method field"));
    }
    cursor += method_len;

    const size_t payload_len = msg_len - cursor;
    RpcPayloadView payload_view;
    if (!payloadViewFromIovecs(iovecs, cursor, payload_len, payload_view)) {
        return std::unexpected(RpcError(RpcErrorCode::DESERIALIZATION_ERROR, "Invalid request payload view"));
    }

    request.requestId(header.m_request_id);
    request.callMode(rpcDecodeCallMode(header.m_flags));
    request.endOfStream(rpcIsEndStream(header.m_flags));
    // 直接move进request，避免再做一次字符串拷贝。
    request.serviceName(std::move(service_name));
    request.methodName(std::move(method_name));
    // 借用RingBuffer中的payload内存，避免额外拷贝。
    // 该视图在后续读取覆盖对应缓冲区前有效。
    request.payloadView(payload_view);
    return msg_len;
}

/**
 * @brief 尝试从iovec数组中解析RPC响应消息
 * @param iovecs iovec数组
 * @param total_readable 可读取的总字节数
 * @param max_message_size 最大消息体大小
 * @param response 输出的响应对象
 * @return 解析消耗的字节数（0表示数据不足），或错误
 */
inline std::expected<size_t, RpcError> tryParseResponseMessage(std::span<const iovec> iovecs,
                                                               size_t total_readable,
                                                               size_t max_message_size,
                                                               RpcResponse& response) {
    if (total_readable < RPC_HEADER_SIZE) {
        return 0;
    }

    char header_buf[RPC_HEADER_SIZE];
    if (!copyFromIovecs(iovecs, 0, header_buf, RPC_HEADER_SIZE)) {
        return std::unexpected(RpcError(RpcErrorCode::INVALID_RESPONSE, "Failed to read response header"));
    }

    RpcHeader header;
    if (!header.deserialize(header_buf)) {
        return std::unexpected(RpcError(RpcErrorCode::INVALID_RESPONSE, "Invalid message header"));
    }

    if (header.m_type != static_cast<uint8_t>(RpcMessageType::RESPONSE)) {
        return std::unexpected(RpcError(RpcErrorCode::INVALID_RESPONSE, "Not a response message"));
    }

    const size_t msg_len = RPC_HEADER_SIZE + header.m_body_length;
    if (msg_len > max_message_size + RPC_HEADER_SIZE) {
        return std::unexpected(RpcError(RpcErrorCode::INVALID_RESPONSE, "Message too large"));
    }

    if (total_readable < msg_len) {
        return 0;
    }

    if (header.m_body_length < sizeof(uint16_t)) {
        return std::unexpected(RpcError(RpcErrorCode::DESERIALIZATION_ERROR, "Response body too short"));
    }

    size_t cursor = RPC_HEADER_SIZE;
    uint16_t error_code_net = 0;
    if (!copyFromIovecs(iovecs, cursor, reinterpret_cast<char*>(&error_code_net), sizeof(error_code_net))) {
        return std::unexpected(RpcError(RpcErrorCode::DESERIALIZATION_ERROR, "Invalid response error code"));
    }
    cursor += sizeof(error_code_net);

    const size_t payload_len = msg_len - cursor;
    RpcPayloadView payload_view;
    if (!payloadViewFromIovecs(iovecs, cursor, payload_len, payload_view)) {
        return std::unexpected(RpcError(RpcErrorCode::DESERIALIZATION_ERROR, "Invalid response payload view"));
    }

    response.requestId(header.m_request_id);
    response.callMode(rpcDecodeCallMode(header.m_flags));
    response.endOfStream(rpcIsEndStream(header.m_flags));
    response.errorCode(static_cast<RpcErrorCode>(rpcNtohs(error_code_net)));
    // 借用RingBuffer中的响应payload内存，按需再materialize。
    response.payloadView(payload_view);
    return msg_len;
}

}  // namespace detail

/**
 * @brief RPC读取器配置
 */
struct RpcReaderSetting {
    size_t max_message_size = RPC_MAX_BODY_SIZE;  ///< 最大消息大小
};

/**
 * @brief RPC写入器配置
 */
struct RpcWriterSetting {
    // 预留扩展
};

/**
 * @brief RPC请求读取等待体（使用readv）
 */
namespace detail {

using RpcAwaitableResult = std::expected<bool, RpcError>;

/**
 * @brief RPC请求读取状态
 *
 * @details 从RingBuffer中逐步读取并解析完整的RPC请求消息。
 */
class RpcRequestReadState : public RpcRingBufferReadStateBase<RpcAwaitableResult>
{
public:
    RpcRequestReadState(RingBuffer& ring_buffer,
                        const RpcReaderSetting& setting,
                        RpcRequest& request)
        : RpcRingBufferReadStateBase<RpcAwaitableResult>(ring_buffer)
        , m_setting(&setting)
        , m_request(&request)
    {
    }

    /// @brief 从RingBuffer中尝试解析请求消息
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
        auto parse_result = tryParseRequestMessage(read_span,
                                                   iovecsReadableBytes(read_span),
                                                   m_setting->max_message_size,
                                                   *m_request);
        if (!parse_result.has_value()) {
            setReadError(parse_result.error());
            return true;
        }

        if (parse_result.value() == 0) {
            return false;
        }

        ringBuffer().consume(parse_result.value());
        return true;
    }

private:
    const RpcReaderSetting* m_setting = nullptr;  ///< 读取配置
    RpcRequest* m_request = nullptr;              ///< 输出请求对象
};

/**
 * @brief RPC响应读取状态
 *
 * @details 从RingBuffer中逐步读取并解析完整的RPC响应消息。
 */
class RpcResponseReadState : public RpcRingBufferReadStateBase<RpcAwaitableResult>
{
public:
    RpcResponseReadState(RingBuffer& ring_buffer,
                         const RpcReaderSetting& setting,
                         RpcResponse& response)
        : RpcRingBufferReadStateBase<RpcAwaitableResult>(ring_buffer)
        , m_setting(&setting)
        , m_response(&response)
    {
    }

    /// @brief 从RingBuffer中尝试解析响应消息
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

        ringBuffer().consume(parse_result.value());
        return true;
    }

private:
    const RpcReaderSetting* m_setting = nullptr;  ///< 读取配置
    RpcResponse* m_response = nullptr;            ///< 输出响应对象
};

/**
 * @brief RPC消息头读取状态
 *
 * @details 仅从RingBuffer中读取并解析RPC消息头（16字节）。
 */
class RpcHeaderReadState : public RpcRingBufferReadStateBase<RpcAwaitableResult>
{
public:
    RpcHeaderReadState(RingBuffer& ring_buffer, RpcHeader& header)
        : RpcRingBufferReadStateBase<RpcAwaitableResult>(ring_buffer)
        , m_header(&header)
    {
    }

    /// @brief 从RingBuffer中尝试解析消息头
    bool parseFromRingBuffer()
    {
        std::array<struct iovec, 2> read_iovecs{};
        const size_t read_iovecs_count = ringBuffer().getReadIovecs(read_iovecs);
        const std::span<const iovec> read_span(read_iovecs.data(), read_iovecs_count);
        if (iovecsReadableBytes(read_span) < RPC_HEADER_SIZE) {
            return false;
        }

        char header_buf[RPC_HEADER_SIZE];
        copyFromIovecs(read_span, 0, header_buf, RPC_HEADER_SIZE);

        if (!m_header->deserialize(header_buf)) {
            setReadError(RpcError(RpcErrorCode::INVALID_REQUEST, "Invalid header"));
            return true;
        }

        ringBuffer().consume(RPC_HEADER_SIZE);
        return true;
    }

private:
    RpcHeader* m_header = nullptr;  ///< 输出消息头
};

/**
 * @brief RPC消息体读取状态
 *
 * @details 从RingBuffer中读取指定长度的消息体数据。
 */
class RpcBodyReadState : public RpcRingBufferReadStateBase<RpcAwaitableResult>
{
public:
    /**
     * @brief 构造消息体读取状态
     * @param ring_buffer 环形缓冲区
     * @param body 输出缓冲区
     * @param body_len 要读取的体长度
     */
    RpcBodyReadState(RingBuffer& ring_buffer, char* body, size_t body_len)
        : RpcRingBufferReadStateBase<RpcAwaitableResult>(ring_buffer)
        , m_body(body)
        , m_body_len(body_len)
    {
    }

    /// @brief 从RingBuffer中尝试读取指定长度的消息体
    bool parseFromRingBuffer()
    {
        std::array<struct iovec, 2> read_iovecs{};
        const size_t read_iovecs_count = ringBuffer().getReadIovecs(read_iovecs);
        const std::span<const iovec> read_span(read_iovecs.data(), read_iovecs_count);
        if (iovecsReadableBytes(read_span) < m_body_len) {
            return false;
        }

        copyFromIovecs(read_span, 0, m_body, m_body_len);
        ringBuffer().consume(m_body_len);
        return true;
    }

private:
    char* m_body = nullptr;      ///< 输出缓冲区
    size_t m_body_len = 0;       ///< 体长度
};

/**
 * @brief RPC请求写入状态
 *
 * @details 将RpcRequest序列化为iovec数组（头部+服务名+方法名+payload），
 *          用于通过writev发送。
 */
class RpcRequestWriteState : public RpcWriteStateBase<RpcAwaitableResult>
{
public:
    /**
     * @brief 构造请求写入状态
     * @param request 要发送的请求对象
     */
    explicit RpcRequestWriteState(const RpcRequest& request)
        : m_request(&request)
    {
        rebuildIovecs();
    }

private:
    /// @brief 重建iovec数组
    void rebuildIovecs()
    {
        RpcPayloadView payload_view = m_request->payloadView();
        const size_t body_size =
            sizeof(uint16_t) + m_request->serviceName().size() +
            sizeof(uint16_t) + m_request->methodName().size() +
            payload_view.size();

        RpcHeader header;
        header.m_type = static_cast<uint8_t>(RpcMessageType::REQUEST);
        header.m_flags = rpcEncodeFlags(m_request->callMode(), m_request->endOfStream());
        header.m_request_id = m_request->requestId();
        header.m_body_length = static_cast<uint32_t>(body_size);
        header.serialize(m_header.data());

        m_service_len = rpcHtons(static_cast<uint16_t>(m_request->serviceName().size()));
        m_method_len = rpcHtons(static_cast<uint16_t>(m_request->methodName().size()));

        auto& iovecs = mutableIovecs();
        iovecs.clear();
        iovecs.reserve(6);
        iovecs.push_back(iovec{m_header.data(), RPC_HEADER_SIZE});
        iovecs.push_back(iovec{&m_service_len, sizeof(m_service_len)});

        if (!m_request->serviceName().empty()) {
            iovecs.push_back(iovec{
                const_cast<char*>(m_request->serviceName().data()),
                m_request->serviceName().size()
            });
        }

        iovecs.push_back(iovec{&m_method_len, sizeof(m_method_len)});
        if (!m_request->methodName().empty()) {
            iovecs.push_back(iovec{
                const_cast<char*>(m_request->methodName().data()),
                m_request->methodName().size()
            });
        }

        if (payload_view.segment1_len > 0) {
            iovecs.push_back(iovec{
                const_cast<char*>(payload_view.segment1),
                payload_view.segment1_len
            });
        }

        if (payload_view.segment2_len > 0) {
            iovecs.push_back(iovec{
                const_cast<char*>(payload_view.segment2),
                payload_view.segment2_len
            });
        }
    }

    const RpcRequest* m_request = nullptr;           ///< 请求对象指针
    std::array<char, RPC_HEADER_SIZE> m_header{};    ///< 序列化后的头部缓冲区
    uint16_t m_service_len = 0;                       ///< 网络字节序的服务名长度
    uint16_t m_method_len = 0;                        ///< 网络字节序的方法名长度
};

/**
 * @brief RPC响应写入状态
 *
 * @details 将RpcResponse序列化为iovec数组（头部+错误码+payload），
 *          用于通过writev发送。
 */
class RpcResponseWriteState : public RpcWriteStateBase<RpcAwaitableResult>
{
public:
    /**
     * @brief 构造响应写入状态
     * @param response 要发送的响应对象
     */
    explicit RpcResponseWriteState(const RpcResponse& response)
        : m_response(&response)
    {
        rebuildIovecs();
    }

private:
    /// @brief 重建iovec数组
    void rebuildIovecs()
    {
        RpcPayloadView payload_view = m_response->payloadView();
        const size_t body_size = sizeof(uint16_t) + payload_view.size();

        RpcHeader header;
        header.m_type = static_cast<uint8_t>(RpcMessageType::RESPONSE);
        header.m_flags = rpcEncodeFlags(m_response->callMode(), m_response->endOfStream());
        header.m_request_id = m_response->requestId();
        header.m_body_length = static_cast<uint32_t>(body_size);
        header.serialize(m_header.data());

        m_error_code = rpcHtons(static_cast<uint16_t>(m_response->errorCode()));

        auto& iovecs = mutableIovecs();
        iovecs.clear();
        iovecs.reserve(4);
        iovecs.push_back(iovec{m_header.data(), RPC_HEADER_SIZE});
        iovecs.push_back(iovec{&m_error_code, sizeof(m_error_code)});

        if (payload_view.segment1_len > 0) {
            iovecs.push_back(iovec{
                const_cast<char*>(payload_view.segment1),
                payload_view.segment1_len
            });
        }

        if (payload_view.segment2_len > 0) {
            iovecs.push_back(iovec{
                const_cast<char*>(payload_view.segment2),
                payload_view.segment2_len
            });
        }
    }

    const RpcResponse* m_response = nullptr;          ///< 响应对象指针
    std::array<char, RPC_HEADER_SIZE> m_header{};     ///< 序列化后的头部缓冲区
    uint16_t m_error_code = 0;                         ///< 网络字节序的错误码
};

}  // namespace detail

/**
 * @brief RPC请求接收等待体（使用readv）
 *
 * @details 支持超时控制的RPC请求接收协程等待体。
 * @tparam SocketType Socket类型
 */
template<typename SocketType>
class GetRpcRequestAwaitable : public TimeoutSupport<GetRpcRequestAwaitable<SocketType>>
{
public:
    using Result = detail::RpcAwaitableResult;

    /**
     * @brief 构造请求接收等待体
     * @param ring_buffer 环形缓冲区
     * @param setting 读取配置
     * @param request 输出请求对象
     * @param socket Socket引用
     */
    GetRpcRequestAwaitable(RingBuffer& ring_buffer,
                           const RpcReaderSetting& setting,
                           RpcRequest& request,
                           SocketType& socket)
        : m_state(std::make_shared<detail::RpcRequestReadState>(ring_buffer, setting, request))
        , m_inner(
            AwaitableBuilder<Result>::fromStateMachine(
                socket.controller(),
                detail::RpcRingBufferReadMachine<detail::RpcRequestReadState>(m_state))
                .build())
    {}

    GetRpcRequestAwaitable(GetRpcRequestAwaitable&&) noexcept = default;
    GetRpcRequestAwaitable& operator=(GetRpcRequestAwaitable&&) noexcept = default;
    GetRpcRequestAwaitable(const GetRpcRequestAwaitable&) = delete;
    GetRpcRequestAwaitable& operator=(const GetRpcRequestAwaitable&) = delete;

    bool await_ready() { return m_inner.await_ready(); }
    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> handle)
    {
        return m_inner.await_suspend(handle);
    }
    Result await_resume() { return m_inner.await_resume(); }
    void markTimeout() { m_inner.markTimeout(); }

private:
    using InnerAwaitable =
        StateMachineAwaitable<detail::RpcRingBufferReadMachine<detail::RpcRequestReadState>>;

    std::shared_ptr<detail::RpcRequestReadState> m_state;  ///< 读取状态
    InnerAwaitable m_inner;  ///< 内部状态机等待体
};

/**
 * @brief RPC响应读取等待体（使用readv）
 *
 * @details 支持超时控制的RPC响应接收协程等待体。
 * @tparam SocketType Socket类型
 */
template<typename SocketType>
class GetRpcResponseAwaitable : public TimeoutSupport<GetRpcResponseAwaitable<SocketType>>
{
public:
    using Result = detail::RpcAwaitableResult;

    /**
     * @brief 构造响应接收等待体
     * @param ring_buffer 环形缓冲区
     * @param setting 读取配置
     * @param response 输出响应对象
     * @param socket Socket引用
     */
    GetRpcResponseAwaitable(RingBuffer& ring_buffer,
                            const RpcReaderSetting& setting,
                            RpcResponse& response,
                            SocketType& socket)
        : m_state(std::make_shared<detail::RpcResponseReadState>(ring_buffer, setting, response))
        , m_inner(
            AwaitableBuilder<Result>::fromStateMachine(
                socket.controller(),
                detail::RpcRingBufferReadMachine<detail::RpcResponseReadState>(m_state))
                .build())
    {}

    GetRpcResponseAwaitable(GetRpcResponseAwaitable&&) noexcept = default;
    GetRpcResponseAwaitable& operator=(GetRpcResponseAwaitable&&) noexcept = default;
    GetRpcResponseAwaitable(const GetRpcResponseAwaitable&) = delete;
    GetRpcResponseAwaitable& operator=(const GetRpcResponseAwaitable&) = delete;

    bool await_ready() { return m_inner.await_ready(); }
    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> handle)
    {
        return m_inner.await_suspend(handle);
    }
    Result await_resume() { return m_inner.await_resume(); }
    void markTimeout() { m_inner.markTimeout(); }

private:
    using InnerAwaitable =
        StateMachineAwaitable<detail::RpcRingBufferReadMachine<detail::RpcResponseReadState>>;

    std::shared_ptr<detail::RpcResponseReadState> m_state;  ///< 读取状态
    InnerAwaitable m_inner;  ///< 内部状态机等待体
};

/**
 * @brief RPC发送请求等待体（使用writev）
 *
 * @details 支持超时控制的RPC请求发送协程等待体。
 * @tparam SocketType Socket类型
 */
template<typename SocketType>
class SendRpcRequestAwaitable : public TimeoutSupport<SendRpcRequestAwaitable<SocketType>>
{
public:
    using Result = detail::RpcAwaitableResult;

    /**
     * @brief 构造请求发送等待体
     * @param request 要发送的请求
     * @param socket Socket引用
     */
    SendRpcRequestAwaitable(const RpcRequest& request, SocketType& socket)
        : m_state(std::make_shared<detail::RpcRequestWriteState>(request))
        , m_inner(
            AwaitableBuilder<Result>::fromStateMachine(
                socket.controller(),
                detail::RpcWritevMachine<detail::RpcRequestWriteState>(m_state))
                .build())
    {}

    SendRpcRequestAwaitable(SendRpcRequestAwaitable&&) noexcept = default;
    SendRpcRequestAwaitable& operator=(SendRpcRequestAwaitable&&) noexcept = default;
    SendRpcRequestAwaitable(const SendRpcRequestAwaitable&) = delete;
    SendRpcRequestAwaitable& operator=(const SendRpcRequestAwaitable&) = delete;

    bool await_ready() { return m_inner.await_ready(); }
    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> handle)
    {
        return m_inner.await_suspend(handle);
    }
    Result await_resume() { return m_inner.await_resume(); }
    void markTimeout() { m_inner.markTimeout(); }

private:
    using InnerAwaitable =
        StateMachineAwaitable<detail::RpcWritevMachine<detail::RpcRequestWriteState>>;

    std::shared_ptr<detail::RpcRequestWriteState> m_state;  ///< 写入状态
    InnerAwaitable m_inner;  ///< 内部状态机等待体
};

/**
 * @brief RPC发送响应等待体（使用writev）
 *
 * @details 支持超时控制的RPC响应发送协程等待体。
 * @tparam SocketType Socket类型
 */
template<typename SocketType>
class SendRpcResponseAwaitable : public TimeoutSupport<SendRpcResponseAwaitable<SocketType>>
{
public:
    using Result = detail::RpcAwaitableResult;

    /**
     * @brief 构造响应发送等待体
     * @param response 要发送的响应
     * @param socket Socket引用
     */
    SendRpcResponseAwaitable(const RpcResponse& response, SocketType& socket)
        : m_state(std::make_shared<detail::RpcResponseWriteState>(response))
        , m_inner(
            AwaitableBuilder<Result>::fromStateMachine(
                socket.controller(),
                detail::RpcWritevMachine<detail::RpcResponseWriteState>(m_state))
                .build())
    {}

    SendRpcResponseAwaitable(SendRpcResponseAwaitable&&) noexcept = default;
    SendRpcResponseAwaitable& operator=(SendRpcResponseAwaitable&&) noexcept = default;
    SendRpcResponseAwaitable(const SendRpcResponseAwaitable&) = delete;
    SendRpcResponseAwaitable& operator=(const SendRpcResponseAwaitable&) = delete;

    bool await_ready() { return m_inner.await_ready(); }
    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> handle)
    {
        return m_inner.await_suspend(handle);
    }
    Result await_resume() { return m_inner.await_resume(); }
    void markTimeout() { m_inner.markTimeout(); }

private:
    using InnerAwaitable =
        StateMachineAwaitable<detail::RpcWritevMachine<detail::RpcResponseWriteState>>;

    std::shared_ptr<detail::RpcResponseWriteState> m_state;  ///< 写入状态
    InnerAwaitable m_inner;  ///< 内部状态机等待体
};

/**
 * @brief 发送原始数据等待体（用于流式传输）
 *
 * @details 将原始字节向量通过writev发送，支持超时控制。
 * @tparam SocketType Socket类型
 */
template<typename SocketType>
class SendRawDataAwaitable : public TimeoutSupport<SendRawDataAwaitable<SocketType>>
{
public:
    using Result = detail::RpcAwaitableResult;

    /**
     * @brief 构造原始数据发送等待体
     * @param data 要发送的数据（移动语义）
     * @param socket Socket引用
     */
    SendRawDataAwaitable(std::vector<char>&& data, SocketType& socket)
        : m_state(std::make_shared<detail::RpcVectorWriteState>(std::move(data)))
        , m_inner(
            AwaitableBuilder<Result>::fromStateMachine(
                socket.controller(),
                detail::RpcWritevMachine<detail::RpcVectorWriteState>(m_state))
                .build())
    {}

    SendRawDataAwaitable(SendRawDataAwaitable&&) noexcept = default;
    SendRawDataAwaitable& operator=(SendRawDataAwaitable&&) noexcept = default;
    SendRawDataAwaitable(const SendRawDataAwaitable&) = delete;
    SendRawDataAwaitable& operator=(const SendRawDataAwaitable&) = delete;

    bool await_ready() { return m_inner.await_ready(); }
    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> handle)
    {
        return m_inner.await_suspend(handle);
    }
    Result await_resume() { return m_inner.await_resume(); }
    void markTimeout() { m_inner.markTimeout(); }

private:
    using InnerAwaitable =
        StateMachineAwaitable<detail::RpcWritevMachine<detail::RpcVectorWriteState>>;

    std::shared_ptr<detail::RpcVectorWriteState> m_state;  ///< 写入状态
    InnerAwaitable m_inner;  ///< 内部状态机等待体
};

/**
 * @brief 读取消息头等待体（用于流式传输）
 *
 * @details 仅读取RPC消息头的协程等待体，支持超时控制。
 * @tparam SocketType Socket类型
 */
template<typename SocketType>
class GetRpcHeaderAwaitable : public TimeoutSupport<GetRpcHeaderAwaitable<SocketType>>
{
public:
    using Result = detail::RpcAwaitableResult;

    /**
     * @brief 构造消息头读取等待体
     * @param ring_buffer 环形缓冲区
     * @param header 输出消息头
     * @param socket Socket引用
     */
    GetRpcHeaderAwaitable(RingBuffer& ring_buffer, RpcHeader& header, SocketType& socket)
        : m_state(std::make_shared<detail::RpcHeaderReadState>(ring_buffer, header))
        , m_inner(
            AwaitableBuilder<Result>::fromStateMachine(
                socket.controller(),
                detail::RpcRingBufferReadMachine<detail::RpcHeaderReadState>(m_state))
                .build())
    {}

    GetRpcHeaderAwaitable(GetRpcHeaderAwaitable&&) noexcept = default;
    GetRpcHeaderAwaitable& operator=(GetRpcHeaderAwaitable&&) noexcept = default;
    GetRpcHeaderAwaitable(const GetRpcHeaderAwaitable&) = delete;
    GetRpcHeaderAwaitable& operator=(const GetRpcHeaderAwaitable&) = delete;

    bool await_ready() { return m_inner.await_ready(); }
    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> handle)
    {
        return m_inner.await_suspend(handle);
    }
    Result await_resume() { return m_inner.await_resume(); }
    void markTimeout() { m_inner.markTimeout(); }

private:
    using InnerAwaitable =
        StateMachineAwaitable<detail::RpcRingBufferReadMachine<detail::RpcHeaderReadState>>;

    std::shared_ptr<detail::RpcHeaderReadState> m_state;  ///< 读取状态
    InnerAwaitable m_inner;  ///< 内部状态机等待体
};

/**
 * @brief 读取消息体等待体（用于流式传输）
 *
 * @details 读取指定长度消息体的协程等待体，支持超时控制。
 * @tparam SocketType Socket类型
 */
template<typename SocketType>
class GetRpcBodyAwaitable : public TimeoutSupport<GetRpcBodyAwaitable<SocketType>>
{
public:
    using Result = detail::RpcAwaitableResult;

    /**
     * @brief 构造消息体读取等待体
     * @param ring_buffer 环形缓冲区
     * @param body 输出缓冲区
     * @param body_len 要读取的体长度
     * @param socket Socket引用
     */
    GetRpcBodyAwaitable(RingBuffer& ring_buffer, char* body, size_t body_len, SocketType& socket)
        : m_state(std::make_shared<detail::RpcBodyReadState>(ring_buffer, body, body_len))
        , m_inner(
            AwaitableBuilder<Result>::fromStateMachine(
                socket.controller(),
                detail::RpcRingBufferReadMachine<detail::RpcBodyReadState>(m_state))
                .build())
    {}

    GetRpcBodyAwaitable(GetRpcBodyAwaitable&&) noexcept = default;
    GetRpcBodyAwaitable& operator=(GetRpcBodyAwaitable&&) noexcept = default;
    GetRpcBodyAwaitable(const GetRpcBodyAwaitable&) = delete;
    GetRpcBodyAwaitable& operator=(const GetRpcBodyAwaitable&) = delete;

    bool await_ready() { return m_inner.await_ready(); }
    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> handle)
    {
        return m_inner.await_suspend(handle);
    }
    Result await_resume() { return m_inner.await_resume(); }
    void markTimeout() { m_inner.markTimeout(); }

private:
    using InnerAwaitable =
        StateMachineAwaitable<detail::RpcRingBufferReadMachine<detail::RpcBodyReadState>>;

    std::shared_ptr<detail::RpcBodyReadState> m_state;  ///< 读取状态
    InnerAwaitable m_inner;  ///< 内部状态机等待体
};

/**
 * @brief RPC读取器模板类
 *
 * @details 提供从连接中读取RPC请求、响应、消息头和消息体的接口。
 * @tparam SocketType Socket类型
 */
template<typename SocketType>
class RpcReaderImpl
{
public:
    using GetHeaderAwaitable = GetRpcHeaderAwaitable<SocketType>;  ///< 消息头读取等待体类型
    using GetBodyAwaitable = GetRpcBodyAwaitable<SocketType>;      ///< 消息体读取等待体类型

    /**
     * @brief 构造读取器
     * @param ring_buffer 环形缓冲区
     * @param setting 读取配置
     * @param socket Socket引用
     */
    RpcReaderImpl(RingBuffer& ring_buffer, const RpcReaderSetting& setting, SocketType& socket)
        : m_ring_buffer(ring_buffer)
        , m_setting(setting)
        , m_socket(socket)
    {
    }

    /**
     * @brief 获取RPC请求
     * @param request 输出请求对象
     * @return 等待体，co_await返回后表示请求已完整解析
     */
    GetRpcRequestAwaitable<SocketType> getRequest(RpcRequest& request) {
        return GetRpcRequestAwaitable<SocketType>(m_ring_buffer, m_setting, request, m_socket);
    }

    /**
     * @brief 获取RPC响应
     * @param response 输出响应对象
     * @return 等待体
     */
    GetRpcResponseAwaitable<SocketType> getResponse(RpcResponse& response) {
        return GetRpcResponseAwaitable<SocketType>(m_ring_buffer, m_setting, response, m_socket);
    }

    /**
     * @brief 获取消息头（用于流式传输）
     */
    GetHeaderAwaitable getHeader(RpcHeader& header) {
        return GetHeaderAwaitable(m_ring_buffer, header, m_socket);
    }

    /**
     * @brief 获取消息体（用于流式传输）
     */
    GetBodyAwaitable getBody(char* body, size_t body_len) {
        return GetBodyAwaitable(m_ring_buffer, body, body_len, m_socket);
    }

private:
    RingBuffer& m_ring_buffer;              ///< 环形缓冲区引用
    const RpcReaderSetting& m_setting;      ///< 读取配置引用
    SocketType& m_socket;                   ///< Socket引用
};

/**
 * @brief RPC写入器模板类
 *
 * @details 提供向连接中发送RPC请求、响应和原始数据的接口。
 * @tparam SocketType Socket类型
 */
template<typename SocketType>
class RpcWriterImpl
{
public:
    using SendRawAwaitable = SendRawDataAwaitable<SocketType>;  ///< 原始数据发送等待体类型

    /**
     * @brief 构造写入器
     * @param setting 写入配置
     * @param socket Socket引用
     */
    RpcWriterImpl(const RpcWriterSetting& setting, SocketType& socket)
        : m_setting(setting)
        , m_socket(socket)
    {
    }

    /**
     * @brief 发送RPC请求
     * @param request 请求对象
     * @return 等待体
     */
    SendRpcRequestAwaitable<SocketType> sendRequest(const RpcRequest& request) {
        return SendRpcRequestAwaitable<SocketType>(request, m_socket);
    }

    /**
     * @brief 发送RPC响应
     * @param response 响应对象
     * @return 等待体
     */
    SendRpcResponseAwaitable<SocketType> sendResponse(const RpcResponse& response) {
        return SendRpcResponseAwaitable<SocketType>(response, m_socket);
    }

    /**
     * @brief 发送原始数据（用于流式传输）
     */
    SendRawAwaitable sendRaw(const char* data, size_t len) {
        std::vector<char> buf(data, data + len);
        return SendRawAwaitable(std::move(buf), m_socket);
    }

private:
    const RpcWriterSetting& m_setting;  ///< 写入配置引用
    SocketType& m_socket;               ///< Socket引用
};

/// @brief RPC读取器类型别名（TcpSocket）
using RpcReader = RpcReaderImpl<TcpSocket>;
/// @brief RPC写入器类型别名（TcpSocket）
using RpcWriter = RpcWriterImpl<TcpSocket>;

/**
 * @brief RPC连接模板类
 *
 * @details 封装TCP连接，使用RingBuffer配合readv/writev提供高效IO。
 */
template<typename SocketType>
class RpcConnImpl
{
public:
    static constexpr size_t kDefaultRingBufferSize = kDefaultRpcRingBufferSize;

    /**
     * @brief 从已有socket构造（服务端使用）
     */
    explicit RpcConnImpl(GHandle handle, const RpcReaderSetting& reader_setting = {},
                         const RpcWriterSetting& writer_setting = {},
                         size_t ring_buffer_size = kDefaultRingBufferSize)
        : m_socket(handle)
        , m_ring_buffer(normalizeRingBufferSize(ring_buffer_size))
        , m_reader_setting(reader_setting)
        , m_writer_setting(writer_setting)
    {
        m_socket.option().handleNonBlock();
    }

    /**
     * @brief 创建新连接（客户端使用）
     */
    explicit RpcConnImpl(IPType type = IPType::IPV4,
                         const RpcReaderSetting& reader_setting = {},
                         const RpcWriterSetting& writer_setting = {},
                         size_t ring_buffer_size = kDefaultRingBufferSize)
        : m_socket(type)
        , m_ring_buffer(normalizeRingBufferSize(ring_buffer_size))
        , m_reader_setting(reader_setting)
        , m_writer_setting(writer_setting)
    {
        m_socket.option().handleNonBlock();
    }

    ~RpcConnImpl() = default;

    // 禁止拷贝
    RpcConnImpl(const RpcConnImpl&) = delete;
    RpcConnImpl& operator=(const RpcConnImpl&) = delete;

    // 允许移动
    RpcConnImpl(RpcConnImpl&&) = default;
    RpcConnImpl& operator=(RpcConnImpl&&) = default;

    /**
     * @brief 连接到服务器
     */
    ConnectAwaitable connect(const Host& host) {
        return m_socket.connect(host);
    }

    /**
     * @brief 获取读取器
     */
    RpcReaderImpl<SocketType> getReader() {
        return RpcReaderImpl<SocketType>(m_ring_buffer, m_reader_setting, m_socket);
    }

    /**
     * @brief 获取写入器
     */
    RpcWriterImpl<SocketType> getWriter() {
        return RpcWriterImpl<SocketType>(m_writer_setting, m_socket);
    }

    /**
     * @brief 获取底层socket
     */
    SocketType& socket() { return m_socket; }

    /**
     * @brief 关闭连接
     */
    CloseAwaitable close() {
        return m_socket.close();
    }

private:
    /// @brief 归一化环形缓冲区大小，0替换为默认值
    static size_t normalizeRingBufferSize(size_t ring_buffer_size) {
        return ring_buffer_size == 0 ? kDefaultRingBufferSize : ring_buffer_size;
    }

    SocketType m_socket;                        ///< 底层Socket
    RingBuffer m_ring_buffer;                   ///< 环形缓冲区
    RpcReaderSetting m_reader_setting;          ///< 读取配置
    RpcWriterSetting m_writer_setting;          ///< 写入配置
};

/// @brief RPC连接类型别名（TcpSocket）
using RpcConn = RpcConnImpl<TcpSocket>;

} // namespace galay::rpc

#endif // GALAY_RPC_CONN_H
