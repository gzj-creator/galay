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
#include "../protoc/rpc_message.h"
#include "../protoc/rpc_error.h"
#include "../../galay-kernel/async/async_tcp.h"
#include "../../galay-kernel/core/awaitable.h"
#include "../../galay-kernel/core/task.h"
#include "../../galay-utils/buffer/ring_buffer.hpp"
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
using ::galay::utils::RingBufferBackendStrategy;
using ::galay::utils::RingBuffer;


// 前向声明
template<typename SocketType, RingBufferBackendStrategy Strategy> class RpcConnImpl;
template<typename SocketType, RingBufferBackendStrategy Strategy> class RpcReaderImpl;
template<typename SocketType> class RpcWriterImpl;

/// @brief RPC连接默认环形缓冲区大小（8KB）
inline constexpr size_t kDefaultRpcRingBufferSize = 8 * 1024;

namespace detail {

/// @brief 计算iovec数组中可读取的总字节数
inline size_t iovecs_readable_bytes(std::span<const iovec> iovecs) {
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
inline bool copy_from_iovecs(std::span<const iovec> iovecs,
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
inline bool payload_view_from_iovecs(std::span<const iovec> iovecs,
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
inline std::expected<size_t, RpcError> try_parse_request_message(std::span<const iovec> iovecs,
                                                              size_t total_readable,
                                                              size_t max_message_size,
                                                              RpcRequest& request) {
    if (total_readable < RPC_HEADER_SIZE) {
        return 0;
    }

    char header_buf[RPC_HEADER_SIZE];
    if (!copy_from_iovecs(iovecs, 0, header_buf, RPC_HEADER_SIZE)) {
        return std::unexpected(RpcError(RpcErrorCode::INVALID_REQUEST, "Failed to read request header"));
    }

    RpcHeader header;
    if (!header.deserialize(header_buf)) {
        return std::unexpected(RpcError(RpcErrorCode::INVALID_REQUEST, "Invalid message header"));
    }

    if (header.m_type != static_cast<uint8_t>(RpcMessageType::REQUEST)) {
        return std::unexpected(RpcError(RpcErrorCode::INVALID_REQUEST, "Not a request message"));
    }
    if ((header.m_reserved & static_cast<uint8_t>(~RPC_RESERVED_KNOWN_MASK)) != 0) {
        return std::unexpected(RpcError(RpcErrorCode::INVALID_REQUEST, "Unsupported request reserved bits"));
    }

    const size_t msg_len = RPC_HEADER_SIZE + header.m_body_length;
    if (msg_len > max_message_size + RPC_HEADER_SIZE) {
        return std::unexpected(RpcError(RpcErrorCode::INVALID_REQUEST, "Message too large"));
    }

    if (total_readable < msg_len) {
        return 0;
    }

    request.request_id(header.m_request_id);
    request.call_mode(rpc_decode_call_mode(header.m_flags));
    request.end_of_stream(rpc_is_end_stream(header.m_flags));
    std::vector<char> body(header.m_body_length);
    if (header.m_body_length > 0 &&
        !copy_from_iovecs(iovecs, RPC_HEADER_SIZE, body.data(), body.size())) {
        return std::unexpected(RpcError(RpcErrorCode::DESERIALIZATION_ERROR, "Invalid request body"));
    }
    if (!request.deserialize_body(body.data(),
                                 body.size(),
                                 (header.m_reserved & RPC_RESERVED_METADATA) != 0)) {
        return std::unexpected(RpcError(RpcErrorCode::DESERIALIZATION_ERROR, "Invalid request body"));
    }
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
inline std::expected<size_t, RpcError> try_parse_response_message(std::span<const iovec> iovecs,
                                                               size_t total_readable,
                                                               size_t max_message_size,
                                                               RpcResponse& response) {
    if (total_readable < RPC_HEADER_SIZE) {
        return 0;
    }

    char header_buf[RPC_HEADER_SIZE];
    if (!copy_from_iovecs(iovecs, 0, header_buf, RPC_HEADER_SIZE)) {
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
    if (!copy_from_iovecs(iovecs, cursor, reinterpret_cast<char*>(&error_code_net), sizeof(error_code_net))) {
        return std::unexpected(RpcError(RpcErrorCode::DESERIALIZATION_ERROR, "Invalid response error code"));
    }
    cursor += sizeof(error_code_net);

    const size_t payload_len = msg_len - cursor;
    RpcPayloadView payload_view;
    if (!payload_view_from_iovecs(iovecs, cursor, payload_len, payload_view)) {
        return std::unexpected(RpcError(RpcErrorCode::DESERIALIZATION_ERROR, "Invalid response payload view"));
    }

    response.request_id(header.m_request_id);
    response.call_mode(rpc_decode_call_mode(header.m_flags));
    response.end_of_stream(rpc_is_end_stream(header.m_flags));
    response.error_code(static_cast<RpcErrorCode>(rpc_ntohs(error_code_net)));
    // 借用RingBuffer中的响应payload内存，按需再materialize。
    response.payload_view(payload_view);
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
template<RingBufferBackendStrategy Strategy = RingBufferBackendStrategy::Mmap>
class RpcRequestReadState : public RpcRingBufferReadStateBase<RpcAwaitableResult, Strategy>
{
public:
    using Base = RpcRingBufferReadStateBase<RpcAwaitableResult, Strategy>;

    RpcRequestReadState(RingBuffer<Strategy, std::dynamic_extent>& ring_buffer,
                        const RpcReaderSetting& setting,
                        RpcRequest& request)
        : Base(ring_buffer)
        , m_setting(&setting)
        , m_request(&request)
    {
    }

    /// @brief 从RingBuffer中尝试解析请求消息
    bool parse_from_ring_buffer()
    {
        if (this->ring_buffer().readable() == 0) {
            return false;
        }

        std::array<struct iovec, 2> read_iovecs{};
        const size_t read_iovecs_count = this->ring_buffer().get_read_iovecs(read_iovecs);
        if (read_iovecs_count == 0) {
            return false;
        }

        const std::span<const iovec> read_span(read_iovecs.data(), read_iovecs_count);
        auto parse_result = try_parse_request_message(read_span,
                                                   iovecs_readable_bytes(read_span),
                                                   m_setting->max_message_size,
                                                   *m_request);
        if (!parse_result.has_value()) {
            this->set_read_error(parse_result.error());
            return true;
        }

        if (parse_result.value() == 0) {
            return false;
        }

        this->ring_buffer().consume(parse_result.value());
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
template<RingBufferBackendStrategy Strategy = RingBufferBackendStrategy::Mmap>
class RpcResponseReadState : public RpcRingBufferReadStateBase<RpcAwaitableResult, Strategy>
{
public:
    using Base = RpcRingBufferReadStateBase<RpcAwaitableResult, Strategy>;

    RpcResponseReadState(RingBuffer<Strategy, std::dynamic_extent>& ring_buffer,
                         const RpcReaderSetting& setting,
                         RpcResponse& response)
        : Base(ring_buffer)
        , m_setting(&setting)
        , m_response(&response)
    {
    }

    /// @brief 从RingBuffer中尝试解析响应消息
    bool parse_from_ring_buffer()
    {
        if (this->ring_buffer().readable() == 0) {
            return false;
        }

        std::array<struct iovec, 2> read_iovecs{};
        const size_t read_iovecs_count = this->ring_buffer().get_read_iovecs(read_iovecs);
        if (read_iovecs_count == 0) {
            return false;
        }

        const std::span<const iovec> read_span(read_iovecs.data(), read_iovecs_count);
        auto parse_result = try_parse_response_message(read_span,
                                                    iovecs_readable_bytes(read_span),
                                                    m_setting->max_message_size,
                                                    *m_response);
        if (!parse_result.has_value()) {
            this->set_read_error(parse_result.error());
            return true;
        }

        if (parse_result.value() == 0) {
            return false;
        }

        this->ring_buffer().consume(parse_result.value());
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
template<RingBufferBackendStrategy Strategy = RingBufferBackendStrategy::Mmap>
class RpcHeaderReadState : public RpcRingBufferReadStateBase<RpcAwaitableResult, Strategy>
{
public:
    using Base = RpcRingBufferReadStateBase<RpcAwaitableResult, Strategy>;

    RpcHeaderReadState(RingBuffer<Strategy, std::dynamic_extent>& ring_buffer, RpcHeader& header)
        : Base(ring_buffer)
        , m_header(&header)
    {
    }

    /// @brief 从RingBuffer中尝试解析消息头
    bool parse_from_ring_buffer()
    {
        std::array<struct iovec, 2> read_iovecs{};
        const size_t read_iovecs_count = this->ring_buffer().get_read_iovecs(read_iovecs);
        const std::span<const iovec> read_span(read_iovecs.data(), read_iovecs_count);
        if (iovecs_readable_bytes(read_span) < RPC_HEADER_SIZE) {
            return false;
        }

        char header_buf[RPC_HEADER_SIZE];
        copy_from_iovecs(read_span, 0, header_buf, RPC_HEADER_SIZE);

        if (!m_header->deserialize(header_buf)) {
            this->set_read_error(RpcError(RpcErrorCode::INVALID_REQUEST, "Invalid header"));
            return true;
        }

        this->ring_buffer().consume(RPC_HEADER_SIZE);
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
template<RingBufferBackendStrategy Strategy = RingBufferBackendStrategy::Mmap>
class RpcBodyReadState : public RpcRingBufferReadStateBase<RpcAwaitableResult, Strategy>
{
public:
    using Base = RpcRingBufferReadStateBase<RpcAwaitableResult, Strategy>;

    /**
     * @brief 构造消息体读取状态
     * @param ring_buffer 环形缓冲区
     * @param body 输出缓冲区
     * @param body_len 要读取的体长度
     */
    RpcBodyReadState(RingBuffer<Strategy, std::dynamic_extent>& ring_buffer, char* body, size_t body_len)
        : Base(ring_buffer)
        , m_body(body)
        , m_body_len(body_len)
    {
    }

    /// @brief 从RingBuffer中尝试读取指定长度的消息体
    bool parse_from_ring_buffer()
    {
        std::array<struct iovec, 2> read_iovecs{};
        const size_t read_iovecs_count = this->ring_buffer().get_read_iovecs(read_iovecs);
        const std::span<const iovec> read_span(read_iovecs.data(), read_iovecs_count);
        if (iovecs_readable_bytes(read_span) < m_body_len) {
            return false;
        }

        copy_from_iovecs(read_span, 0, m_body, m_body_len);
        this->ring_buffer().consume(m_body_len);
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
        rebuild_iovecs();
    }

private:
    RpcRequestWriteState(const RpcRequestWriteState&) = delete;
    RpcRequestWriteState& operator=(const RpcRequestWriteState&) = delete;
public:
    RpcRequestWriteState(RpcRequestWriteState&&) = delete;
    RpcRequestWriteState& operator=(RpcRequestWriteState&&) = delete;

private:
    /// @brief 重建iovec数组
    void rebuild_iovecs()
    {
        auto validation = m_request->validate_for_write();
        if (!validation.has_value()) {
            set_write_error(validation.error());
            return;
        }

        RpcPayloadView payload_view = m_request->payload_view();
        const size_t body_size = m_request->serialized_body_size();

        RpcHeader header;
        header.m_type = static_cast<uint8_t>(RpcMessageType::REQUEST);
        header.m_flags = rpc_encode_flags(m_request->call_mode(), m_request->end_of_stream());
        if (!m_request->metadata().empty()) {
            header.m_reserved = RPC_RESERVED_METADATA;
        }
        header.m_request_id = m_request->request_id();
        header.m_body_length = static_cast<uint32_t>(body_size);
        header.serialize(m_header.data());

        m_service_len = rpc_htons(static_cast<uint16_t>(m_request->service_name().size()));
        m_method_len = rpc_htons(static_cast<uint16_t>(m_request->method_name().size()));
        rebuild_metadata_buffer();

        auto& iovecs = mutable_iovecs();
        iovecs.clear();
        iovecs.reserve(7);
        iovecs.push_back(iovec{m_header.data(), RPC_HEADER_SIZE});
        if (!m_metadata.empty()) {
            iovecs.push_back(iovec{m_metadata.data(), m_metadata.size()});
        }
        iovecs.push_back(iovec{&m_service_len, sizeof(m_service_len)});

        if (!m_request->service_name().empty()) {
            iovecs.push_back(iovec{
                const_cast<char*>(m_request->service_name().data()),
                m_request->service_name().size()
            });
        }

        iovecs.push_back(iovec{&m_method_len, sizeof(m_method_len)});
        if (!m_request->method_name().empty()) {
            iovecs.push_back(iovec{
                const_cast<char*>(m_request->method_name().data()),
                m_request->method_name().size()
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

    void rebuild_metadata_buffer()
    {
        m_metadata.clear();
        if (m_request->metadata().empty()) {
            return;
        }

        size_t metadata_size = sizeof(uint16_t) + sizeof(uint16_t);
        for (const auto& [key, value] : m_request->metadata()) {
            metadata_size += sizeof(uint16_t) + sizeof(uint16_t) + key.size() + value.size();
        }
        m_metadata.resize(metadata_size);

        size_t offset = 0;
        uint16_t marker = rpc_htons(kRpcRequestMetadataMarker);
        uint16_t count = rpc_htons(static_cast<uint16_t>(m_request->metadata().size()));
        std::memcpy(m_metadata.data() + offset, &marker, sizeof(marker));
        offset += sizeof(marker);
        std::memcpy(m_metadata.data() + offset, &count, sizeof(count));
        offset += sizeof(count);
        for (const auto& [key, value] : m_request->metadata()) {
            uint16_t key_len = rpc_htons(static_cast<uint16_t>(key.size()));
            uint16_t value_len = rpc_htons(static_cast<uint16_t>(value.size()));
            std::memcpy(m_metadata.data() + offset, &key_len, sizeof(key_len));
            offset += sizeof(key_len);
            std::memcpy(m_metadata.data() + offset, &value_len, sizeof(value_len));
            offset += sizeof(value_len);
            std::memcpy(m_metadata.data() + offset, key.data(), key.size());
            offset += key.size();
            std::memcpy(m_metadata.data() + offset, value.data(), value.size());
            offset += value.size();
        }
    }

    const RpcRequest* m_request = nullptr;           ///< 请求对象指针
    std::array<char, RPC_HEADER_SIZE> m_header{};    ///< 序列化后的头部缓冲区
    std::vector<char> m_metadata;                     ///< 序列化后的metadata缓冲区
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
        rebuild_iovecs();
    }

private:
    RpcResponseWriteState(const RpcResponseWriteState&) = delete;
    RpcResponseWriteState& operator=(const RpcResponseWriteState&) = delete;
public:
    RpcResponseWriteState(RpcResponseWriteState&&) = delete;
    RpcResponseWriteState& operator=(RpcResponseWriteState&&) = delete;

private:
    /// @brief 重建iovec数组
    void rebuild_iovecs()
    {
        auto validation = m_response->validate_for_write();
        if (!validation.has_value()) {
            set_write_error(validation.error());
            return;
        }

        RpcPayloadView payload_view = m_response->payload_view();
        const size_t body_size = sizeof(uint16_t) + payload_view.size();

        RpcHeader header;
        header.m_type = static_cast<uint8_t>(RpcMessageType::RESPONSE);
        header.m_flags = rpc_encode_flags(m_response->call_mode(), m_response->end_of_stream());
        header.m_request_id = m_response->request_id();
        header.m_body_length = static_cast<uint32_t>(body_size);
        header.serialize(m_header.data());

        m_error_code = rpc_htons(static_cast<uint16_t>(m_response->error_code()));

        auto& iovecs = mutable_iovecs();
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
template<typename SocketType, RingBufferBackendStrategy Strategy = RingBufferBackendStrategy::Mmap>
class GetRpcRequestAwaitable
    : public ForwardingAwaitable<GetRpcRequestAwaitable<SocketType, Strategy>>
    , public TimeoutSupport<GetRpcRequestAwaitable<SocketType, Strategy>>
{
public:
    friend class ForwardingAwaitable<GetRpcRequestAwaitable<SocketType, Strategy>>;
    using Result = detail::RpcAwaitableResult;
    using ReadState = detail::RpcRequestReadState<Strategy>;

    /**
     * @brief 构造请求接收等待体
     * @param ring_buffer 环形缓冲区
     * @param setting 读取配置
     * @param request 输出请求对象
     * @param socket Socket引用
     */
    GetRpcRequestAwaitable(RingBuffer<Strategy, std::dynamic_extent>& ring_buffer,
                           const RpcReaderSetting& setting,
                           RpcRequest& request,
                           SocketType& socket)
        : m_state(std::make_shared<ReadState>(ring_buffer, setting, request))
        , m_inner(
            AwaitableBuilder<Result>::from_state_machine(
                socket.controller(),
                detail::RpcRingBufferReadMachine<ReadState>(m_state))
                .build())
    {}

    GetRpcRequestAwaitable(GetRpcRequestAwaitable&&) noexcept = default;
    GetRpcRequestAwaitable& operator=(GetRpcRequestAwaitable&&) noexcept = default;
    GetRpcRequestAwaitable(const GetRpcRequestAwaitable&) = delete;
    GetRpcRequestAwaitable& operator=(const GetRpcRequestAwaitable&) = delete;

private:
    using InnerAwaitable =
        StateMachineAwaitable<detail::RpcRingBufferReadMachine<ReadState>>;

    std::shared_ptr<ReadState> m_state;  ///< 读取状态

private:
    InnerAwaitable m_inner;  ///< 内部状态机等待体
};

/**
 * @brief RPC响应读取等待体（使用readv）
 *
 * @details 支持超时控制的RPC响应接收协程等待体。
 * @tparam SocketType Socket类型
 */
template<typename SocketType, RingBufferBackendStrategy Strategy = RingBufferBackendStrategy::Mmap>
class GetRpcResponseAwaitable
    : public ForwardingAwaitable<GetRpcResponseAwaitable<SocketType, Strategy>>
    , public TimeoutSupport<GetRpcResponseAwaitable<SocketType, Strategy>>
{
public:
    friend class ForwardingAwaitable<GetRpcResponseAwaitable<SocketType, Strategy>>;
    using Result = detail::RpcAwaitableResult;
    using ReadState = detail::RpcResponseReadState<Strategy>;

    /**
     * @brief 构造响应接收等待体
     * @param ring_buffer 环形缓冲区
     * @param setting 读取配置
     * @param response 输出响应对象
     * @param socket Socket引用
     */
    GetRpcResponseAwaitable(RingBuffer<Strategy, std::dynamic_extent>& ring_buffer,
                            const RpcReaderSetting& setting,
                            RpcResponse& response,
                            SocketType& socket)
        : m_state(std::make_shared<ReadState>(ring_buffer, setting, response))
        , m_inner(
            AwaitableBuilder<Result>::from_state_machine(
                socket.controller(),
                detail::RpcRingBufferReadMachine<ReadState>(m_state))
                .build())
    {}

    GetRpcResponseAwaitable(GetRpcResponseAwaitable&&) noexcept = default;
    GetRpcResponseAwaitable& operator=(GetRpcResponseAwaitable&&) noexcept = default;
    GetRpcResponseAwaitable(const GetRpcResponseAwaitable&) = delete;
    GetRpcResponseAwaitable& operator=(const GetRpcResponseAwaitable&) = delete;

private:
    using InnerAwaitable =
        StateMachineAwaitable<detail::RpcRingBufferReadMachine<ReadState>>;

    std::shared_ptr<ReadState> m_state;  ///< 读取状态

private:
    InnerAwaitable m_inner;  ///< 内部状态机等待体
};

/**
 * @brief RPC发送请求等待体（使用writev）
 *
 * @details 支持超时控制的RPC请求发送协程等待体。
 * @tparam SocketType Socket类型
 */
template<typename SocketType>
class SendRpcRequestAwaitable
    : public ForwardingAwaitable<SendRpcRequestAwaitable<SocketType>>
    , public TimeoutSupport<SendRpcRequestAwaitable<SocketType>>
{
public:
    friend class ForwardingAwaitable<SendRpcRequestAwaitable<SocketType>>;
    using Result = detail::RpcAwaitableResult;

    /**
     * @brief 构造请求发送等待体
     * @param request 要发送的请求
     * @param socket Socket引用
     */
    SendRpcRequestAwaitable(const RpcRequest& request, SocketType& socket)
        : m_state(std::make_shared<detail::RpcRequestWriteState>(request))
        , m_inner(
            AwaitableBuilder<Result>::from_state_machine(
                socket.controller(),
                detail::RpcWritevMachine<detail::RpcRequestWriteState>(m_state))
                .build())
    {}

    SendRpcRequestAwaitable(SendRpcRequestAwaitable&&) noexcept = default;
    SendRpcRequestAwaitable& operator=(SendRpcRequestAwaitable&&) noexcept = default;
    SendRpcRequestAwaitable(const SendRpcRequestAwaitable&) = delete;
    SendRpcRequestAwaitable& operator=(const SendRpcRequestAwaitable&) = delete;

private:
    using InnerAwaitable =
        StateMachineAwaitable<detail::RpcWritevMachine<detail::RpcRequestWriteState>>;

    std::shared_ptr<detail::RpcRequestWriteState> m_state;  ///< 写入状态

private:
    InnerAwaitable m_inner;  ///< 内部状态机等待体
};

/**
 * @brief RPC发送响应等待体（使用writev）
 *
 * @details 支持超时控制的RPC响应发送协程等待体。
 * @tparam SocketType Socket类型
 */
template<typename SocketType>
class SendRpcResponseAwaitable
    : public ForwardingAwaitable<SendRpcResponseAwaitable<SocketType>>
    , public TimeoutSupport<SendRpcResponseAwaitable<SocketType>>
{
public:
    friend class ForwardingAwaitable<SendRpcResponseAwaitable<SocketType>>;
    using Result = detail::RpcAwaitableResult;

    /**
     * @brief 构造响应发送等待体
     * @param response 要发送的响应
     * @param socket Socket引用
     */
    SendRpcResponseAwaitable(const RpcResponse& response, SocketType& socket)
        : m_state(std::make_shared<detail::RpcResponseWriteState>(response))
        , m_inner(
            AwaitableBuilder<Result>::from_state_machine(
                socket.controller(),
                detail::RpcWritevMachine<detail::RpcResponseWriteState>(m_state))
                .build())
    {}

    SendRpcResponseAwaitable(SendRpcResponseAwaitable&&) noexcept = default;
    SendRpcResponseAwaitable& operator=(SendRpcResponseAwaitable&&) noexcept = default;
    SendRpcResponseAwaitable(const SendRpcResponseAwaitable&) = delete;
    SendRpcResponseAwaitable& operator=(const SendRpcResponseAwaitable&) = delete;

private:
    using InnerAwaitable =
        StateMachineAwaitable<detail::RpcWritevMachine<detail::RpcResponseWriteState>>;

    std::shared_ptr<detail::RpcResponseWriteState> m_state;  ///< 写入状态

private:
    InnerAwaitable m_inner;  ///< 内部状态机等待体
};

/**
 * @brief 发送原始数据等待体（用于流式传输）
 *
 * @details 将原始字节向量通过writev发送，支持超时控制。
 * @tparam SocketType Socket类型
 */
template<typename SocketType>
class SendRawDataAwaitable
    : public ForwardingAwaitable<SendRawDataAwaitable<SocketType>>
    , public TimeoutSupport<SendRawDataAwaitable<SocketType>>
{
public:
    friend class ForwardingAwaitable<SendRawDataAwaitable<SocketType>>;
    using Result = detail::RpcAwaitableResult;

    /**
     * @brief 构造原始数据发送等待体
     * @param data 要发送的数据（移动语义）
     * @param socket Socket引用
     */
    SendRawDataAwaitable(std::vector<char>&& data, SocketType& socket)
        : m_state(std::make_shared<detail::RpcVectorWriteState>(std::move(data)))
        , m_inner(
            AwaitableBuilder<Result>::from_state_machine(
                socket.controller(),
                detail::RpcWritevMachine<detail::RpcVectorWriteState>(m_state))
                .build())
    {}

    SendRawDataAwaitable(SendRawDataAwaitable&&) noexcept = default;
    SendRawDataAwaitable& operator=(SendRawDataAwaitable&&) noexcept = default;
    SendRawDataAwaitable(const SendRawDataAwaitable&) = delete;
    SendRawDataAwaitable& operator=(const SendRawDataAwaitable&) = delete;

private:
    using InnerAwaitable =
        StateMachineAwaitable<detail::RpcWritevMachine<detail::RpcVectorWriteState>>;

    std::shared_ptr<detail::RpcVectorWriteState> m_state;  ///< 写入状态

private:
    InnerAwaitable m_inner;  ///< 内部状态机等待体
};

/**
 * @brief 读取消息头等待体（用于流式传输）
 *
 * @details 仅读取RPC消息头的协程等待体，支持超时控制。
 * @tparam SocketType Socket类型
 */
template<typename SocketType, RingBufferBackendStrategy Strategy = RingBufferBackendStrategy::Mmap>
class GetRpcHeaderAwaitable
    : public ForwardingAwaitable<GetRpcHeaderAwaitable<SocketType, Strategy>>
    , public TimeoutSupport<GetRpcHeaderAwaitable<SocketType, Strategy>>
{
public:
    friend class ForwardingAwaitable<GetRpcHeaderAwaitable<SocketType, Strategy>>;
    using Result = detail::RpcAwaitableResult;
    using ReadState = detail::RpcHeaderReadState<Strategy>;

    /**
     * @brief 构造消息头读取等待体
     * @param ring_buffer 环形缓冲区
     * @param header 输出消息头
     * @param socket Socket引用
     */
    GetRpcHeaderAwaitable(RingBuffer<Strategy, std::dynamic_extent>& ring_buffer, RpcHeader& header, SocketType& socket)
        : m_state(std::make_shared<ReadState>(ring_buffer, header))
        , m_inner(
            AwaitableBuilder<Result>::from_state_machine(
                socket.controller(),
                detail::RpcRingBufferReadMachine<ReadState>(m_state))
                .build())
    {}

    GetRpcHeaderAwaitable(GetRpcHeaderAwaitable&&) noexcept = default;
    GetRpcHeaderAwaitable& operator=(GetRpcHeaderAwaitable&&) noexcept = default;
    GetRpcHeaderAwaitable(const GetRpcHeaderAwaitable&) = delete;
    GetRpcHeaderAwaitable& operator=(const GetRpcHeaderAwaitable&) = delete;

private:
    using InnerAwaitable =
        StateMachineAwaitable<detail::RpcRingBufferReadMachine<ReadState>>;

    std::shared_ptr<ReadState> m_state;  ///< 读取状态

private:
    InnerAwaitable m_inner;  ///< 内部状态机等待体
};

/**
 * @brief 读取消息体等待体（用于流式传输）
 *
 * @details 读取指定长度消息体的协程等待体，支持超时控制。
 * @tparam SocketType Socket类型
 */
template<typename SocketType, RingBufferBackendStrategy Strategy = RingBufferBackendStrategy::Mmap>
class GetRpcBodyAwaitable
    : public ForwardingAwaitable<GetRpcBodyAwaitable<SocketType, Strategy>>
    , public TimeoutSupport<GetRpcBodyAwaitable<SocketType, Strategy>>
{
public:
    friend class ForwardingAwaitable<GetRpcBodyAwaitable<SocketType, Strategy>>;
    using Result = detail::RpcAwaitableResult;
    using ReadState = detail::RpcBodyReadState<Strategy>;

    /**
     * @brief 构造消息体读取等待体
     * @param ring_buffer 环形缓冲区
     * @param body 输出缓冲区
     * @param body_len 要读取的体长度
     * @param socket Socket引用
     */
    GetRpcBodyAwaitable(RingBuffer<Strategy, std::dynamic_extent>& ring_buffer, char* body, size_t body_len, SocketType& socket)
        : m_state(std::make_shared<ReadState>(ring_buffer, body, body_len))
        , m_inner(
            AwaitableBuilder<Result>::from_state_machine(
                socket.controller(),
                detail::RpcRingBufferReadMachine<ReadState>(m_state))
                .build())
    {}

    GetRpcBodyAwaitable(GetRpcBodyAwaitable&&) noexcept = default;
    GetRpcBodyAwaitable& operator=(GetRpcBodyAwaitable&&) noexcept = default;
    GetRpcBodyAwaitable(const GetRpcBodyAwaitable&) = delete;
    GetRpcBodyAwaitable& operator=(const GetRpcBodyAwaitable&) = delete;

private:
    using InnerAwaitable =
        StateMachineAwaitable<detail::RpcRingBufferReadMachine<ReadState>>;

    std::shared_ptr<ReadState> m_state;  ///< 读取状态

private:
    InnerAwaitable m_inner;  ///< 内部状态机等待体
};

/**
 * @brief RPC读取器模板类
 *
 * @details 提供从连接中读取RPC请求、响应、消息头和消息体的接口。
 * @tparam SocketType Socket类型
 */
template<typename SocketType, RingBufferBackendStrategy Strategy = RingBufferBackendStrategy::Mmap>
class RpcReaderImpl
{
public:
    using GetHeaderAwaitable = GetRpcHeaderAwaitable<SocketType, Strategy>;  ///< 消息头读取等待体类型
    using GetBodyAwaitable = GetRpcBodyAwaitable<SocketType, Strategy>;      ///< 消息体读取等待体类型

    /**
     * @brief 构造读取器
     * @param ring_buffer 环形缓冲区
     * @param setting 读取配置
     * @param socket Socket引用
     */
    RpcReaderImpl(RingBuffer<Strategy, std::dynamic_extent>& ring_buffer, const RpcReaderSetting& setting, SocketType& socket)
        : m_ring_buffer(ring_buffer)
        , m_setting(setting)
        , m_socket(socket)
    {
    }

private:
    RpcReaderImpl(const RpcReaderImpl&) = delete;
    RpcReaderImpl& operator=(const RpcReaderImpl&) = delete;
public:

    RpcReaderImpl(RpcReaderImpl&& other) noexcept
        : m_ring_buffer(other.m_ring_buffer)
        , m_setting(other.m_setting)
        , m_socket(other.m_socket)
    {
    }

    RpcReaderImpl& operator=(RpcReaderImpl&&) = delete;

    /**
     * @brief 获取RPC请求
     * @param request 输出请求对象
     * @return 等待体，co_await返回后表示请求已完整解析
     */
    GetRpcRequestAwaitable<SocketType, Strategy> get_request(RpcRequest& request) {
        return GetRpcRequestAwaitable<SocketType, Strategy>(m_ring_buffer, m_setting, request, m_socket);
    }

    /**
     * @brief 获取RPC响应
     * @param response 输出响应对象
     * @return 等待体
     */
    GetRpcResponseAwaitable<SocketType, Strategy> get_response(RpcResponse& response) {
        return GetRpcResponseAwaitable<SocketType, Strategy>(m_ring_buffer, m_setting, response, m_socket);
    }

    /**
     * @brief 获取消息头（用于流式传输）
     */
    GetHeaderAwaitable get_header(RpcHeader& header) {
        return GetHeaderAwaitable(m_ring_buffer, header, m_socket);
    }

    /**
     * @brief 获取消息体（用于流式传输）
     */
    GetBodyAwaitable get_body(char* body, size_t body_len) {
        return GetBodyAwaitable(m_ring_buffer, body, body_len, m_socket);
    }

private:
    RingBuffer<Strategy, std::dynamic_extent>& m_ring_buffer;    ///< 环形缓冲区引用
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

private:
    RpcWriterImpl(const RpcWriterImpl&) = delete;
    RpcWriterImpl& operator=(const RpcWriterImpl&) = delete;
public:

    RpcWriterImpl(RpcWriterImpl&& other) noexcept
        : m_setting(other.m_setting)
        , m_socket(other.m_socket)
    {
    }

    RpcWriterImpl& operator=(RpcWriterImpl&&) = delete;

    /**
     * @brief 发送RPC请求
     * @param request 请求对象
     * @return 等待体
     */
    SendRpcRequestAwaitable<SocketType> send_request(const RpcRequest& request) {
        return SendRpcRequestAwaitable<SocketType>(request, m_socket);
    }

    /**
     * @brief 发送RPC响应
     * @param response 响应对象
     * @return 等待体
     */
    SendRpcResponseAwaitable<SocketType> send_response(const RpcResponse& response) {
        return SendRpcResponseAwaitable<SocketType>(response, m_socket);
    }

    /**
     * @brief 发送原始数据（用于流式传输）
     */
    SendRawAwaitable send_raw(const char* data, size_t len) {
        std::vector<char> buf(data, data + len);
        return SendRawAwaitable(std::move(buf), m_socket);
    }

private:
    const RpcWriterSetting& m_setting;  ///< 写入配置引用
    SocketType& m_socket;               ///< Socket引用
};

/// @brief RPC读取器类型别名（AsyncTcpSocket）
using RpcReader = RpcReaderImpl<AsyncTcpSocket>;
/// @brief RPC写入器类型别名（AsyncTcpSocket）
using RpcWriter = RpcWriterImpl<AsyncTcpSocket>;

/**
 * @brief RPC连接模板类
 *
 * @details 封装TCP连接，使用RingBuffer配合readv/writev提供高效IO。
 */
template<typename SocketType, RingBufferBackendStrategy Strategy = RingBufferBackendStrategy::Mmap>
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
        , m_ring_buffer(normalize_ring_buffer_size(ring_buffer_size))
        , m_reader_setting(reader_setting)
        , m_writer_setting(writer_setting)
    {
        m_socket.option().handle_non_block();
    }

    /**
     * @brief 创建新连接（客户端使用）
     */
    explicit RpcConnImpl(IPType type = IPType::IPV4,
                         const RpcReaderSetting& reader_setting = {},
                         const RpcWriterSetting& writer_setting = {},
                         size_t ring_buffer_size = kDefaultRingBufferSize)
        : m_socket(type)
        , m_ring_buffer(normalize_ring_buffer_size(ring_buffer_size))
        , m_reader_setting(reader_setting)
        , m_writer_setting(writer_setting)
    {
        m_socket.option().handle_non_block();
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
    RpcReaderImpl<SocketType, Strategy> get_reader() {
        return RpcReaderImpl<SocketType, Strategy>(m_ring_buffer, m_reader_setting, m_socket);
    }

    /**
     * @brief 获取写入器
     */
    RpcWriterImpl<SocketType> get_writer() {
        return RpcWriterImpl<SocketType>(m_writer_setting, m_socket);
    }

    /**
     * @brief 获取底层socket
     */
    SocketType& socket() { return m_socket; }

    /**
     * @brief 获取RingBuffer
     */
    RingBuffer<Strategy, std::dynamic_extent>& ring_buffer() { return m_ring_buffer; }

    /**
     * @brief 关闭连接
     */
    CloseAwaitable close() {
        return m_socket.close();
    }

private:
    /// @brief 归一化环形缓冲区大小，0替换为默认值
    static size_t normalize_ring_buffer_size(size_t ring_buffer_size) {
        return ring_buffer_size == 0 ? kDefaultRingBufferSize : ring_buffer_size;
    }

    SocketType m_socket;                        ///< 底层Socket
    RingBuffer<Strategy, std::dynamic_extent> m_ring_buffer;         ///< 环形缓冲区
    RpcReaderSetting m_reader_setting;          ///< 读取配置
    RpcWriterSetting m_writer_setting;          ///< 写入配置
};

/// @brief RPC连接类型别名（AsyncTcpSocket）
using RpcConn = RpcConnImpl<AsyncTcpSocket>;

} // namespace galay::rpc

#endif // GALAY_RPC_CONN_H
