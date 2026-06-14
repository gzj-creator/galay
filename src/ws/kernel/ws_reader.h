/**
 * @file ws_reader.h
 * @brief WebSocket 读取器，基于异步状态机从 Socket 读取并解析 WebSocket 帧
 * @author galay-http
 * @version 1.0.0
 *
 * @details 提供 WsReaderImpl 模板类，支持从 TcpSocket 或 SslSocket 读取
 *          WebSocket 帧和消息。内部使用快速路径优化单帧消息的解析。
 *          支持 UTF-8 校验、掩码解掩和分片消息重组。
 */

#ifndef GALAY_WS_READER_H
#define GALAY_WS_READER_H

#include "reader_cfg.h"
#include "http/common/iovec_utils.h"
#include "ws/protoc/ws_error.h"
#include "ws/protoc/ws_frame.h"
#include "kernel/async/tcp_socket.h"
#include <utils/cache/bytes.hpp>
#include <utils/cache/ring_buffer.hpp>
#include "kernel/kernel/awaitable.h"
#include <expected>
#include <limits>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <cstring>

#ifdef GALAY_SSL_FEATURE_ENABLED
#include "ssl/async/ssl_await.h"
#include "ssl/async/ssl_socket.h"
#endif

namespace galay::websocket {

using namespace galay::async;
using namespace galay::kernel;
using ::galay::utils::Bytes;
using ::galay::utils::RingBuffer;

template<typename T>
struct is_ssl_socket : std::false_type {};

#ifdef GALAY_SSL_FEATURE_ENABLED
template<>
struct is_ssl_socket<galay::ssl::SslSocket> : std::true_type {};
#endif

template<typename T>
inline constexpr bool is_ssl_socket_v = is_ssl_socket<T>::value;

/**
 * @brief 控制帧回调函数类型
 * @param opcode 帧操作码
 * @param payload 帧负载
 */
using ControlFrameCallback = std::function<void(WsOpcode opcode, const std::string& payload)>;

namespace detail {

/**
 * @brief WebSocket 消息快速路径状态
 */
enum class WsMessageFastPathStatus {
    kFallback,  ///< 回退到常规解析路径
    kNeedMore,  ///< 需要更多数据
    kContinue,  ///< 继续解析下一个帧
    kReturn,    ///< 解析完成，返回结果
};

struct WsFastPathFramePrefix {
    bool fin = false;
    WsOpcode opcode = WsOpcode::Text;
    bool mask = false;
    uint64_t payload_length = 0;
    uint8_t masking_key[4] = {0, 0, 0, 0};
    size_t payload_offset = 0;
    size_t frame_length = 0;
};

struct WsFastPathPrefixResult {
    WsMessageFastPathStatus status = WsMessageFastPathStatus::kFallback;
    WsFastPathFramePrefix prefix;
};

struct WsConsumeFastPathView {
    WsOpcode opcode = WsOpcode::Text;
    size_t payload_length = 0;
    size_t payload_offset = 0;
    size_t frame_length = 0;
    char* payload_data = nullptr;
    uint8_t masking_key[4] = {0, 0, 0, 0};
    BorrowedIovecs<2> payload_iovecs;
};

inline size_t wsIovecTotalLength(const struct iovec* iovecs, size_t iovec_count) noexcept {
    size_t total = 0;
    for (size_t i = 0; i < iovec_count; ++i) {
        total += iovecs[i].iov_len;
    }
    return total;
}

inline bool wsReadIovecBytes(const struct iovec* iovecs,
                             size_t iovec_count,
                             size_t offset,
                             void* dst,
                             size_t len) noexcept {
    if (len == 0) {
        return true;
    }

    auto* out = static_cast<uint8_t*>(dst);
    size_t remaining = len;
    size_t cursor = offset;

    for (size_t i = 0; i < iovec_count; ++i) {
        const auto& iov = iovecs[i];
        if (cursor >= iov.iov_len) {
            cursor -= iov.iov_len;
            continue;
        }

        const size_t available = iov.iov_len - cursor;
        const size_t take = std::min(available, remaining);
        std::memcpy(out,
                    static_cast<const uint8_t*>(iov.iov_base) + cursor,
                    take);
        out += take;
        remaining -= take;
        cursor = 0;
        if (remaining == 0) {
            return true;
        }
    }

    return false;
}

inline void wsApplyMaskInPlace(char* data, size_t len, const uint8_t masking_key[4]) noexcept {
    WsFrameParser::applyMaskBytes(data, len, masking_key);
}

inline bool wsIsValidUtf8Span(const char* data, size_t len) noexcept {
    return WsFrameParser::isValidUtf8Bytes(data, len);
}

inline bool wsIsValidUtf8MaskedSpan(const char* data,
                                    size_t len,
                                    const uint8_t masking_key[4]) noexcept {
    return WsFrameParser::isValidUtf8MaskedBytes(data, len, masking_key);
}

inline bool wsIsValidUtf8MaskedIovecs(const struct iovec* iovecs,
                                      size_t iovec_count,
                                      const uint8_t masking_key[4]) noexcept {
    size_t total_length = 0;
    for (size_t i = 0; i < iovec_count; ++i) {
        total_length += iovecs[i].iov_len;
    }

    size_t logical_index = 0;
    auto masked = [&](size_t absolute_index) noexcept -> uint8_t {
        size_t cursor = absolute_index;
        for (size_t i = 0; i < iovec_count; ++i) {
            if (cursor < iovecs[i].iov_len) {
                return static_cast<uint8_t>(
                    static_cast<const char*>(iovecs[i].iov_base)[cursor]) ^
                    masking_key[absolute_index % 4];
            }
            cursor -= iovecs[i].iov_len;
        }
        return 0;
    };

    while (logical_index < total_length) {
        const uint8_t byte = masked(logical_index);
        if (byte <= 0x7F) {
            ++logical_index;
            continue;
        }

        if ((byte & 0xE0) == 0xC0) {
            if (logical_index + 1 >= total_length) return false;
            const uint8_t byte2 = masked(logical_index + 1);
            if ((byte2 & 0xC0) != 0x80) return false;
            const uint32_t codepoint = ((byte & 0x1F) << 6) | (byte2 & 0x3F);
            if (codepoint < 0x80) return false;
            logical_index += 2;
            continue;
        }

        if ((byte & 0xF0) == 0xE0) {
            if (logical_index + 2 >= total_length) return false;
            const uint8_t byte2 = masked(logical_index + 1);
            const uint8_t byte3 = masked(logical_index + 2);
            if ((byte2 & 0xC0) != 0x80) return false;
            if ((byte3 & 0xC0) != 0x80) return false;
            const uint32_t codepoint = ((byte & 0x0F) << 12) |
                                       ((byte2 & 0x3F) << 6) |
                                       (byte3 & 0x3F);
            if (codepoint < 0x800) return false;
            if (codepoint >= 0xD800 && codepoint <= 0xDFFF) return false;
            logical_index += 3;
            continue;
        }

        if ((byte & 0xF8) == 0xF0) {
            if (logical_index + 3 >= total_length) return false;
            const uint8_t byte2 = masked(logical_index + 1);
            const uint8_t byte3 = masked(logical_index + 2);
            const uint8_t byte4 = masked(logical_index + 3);
            if ((byte2 & 0xC0) != 0x80) return false;
            if ((byte3 & 0xC0) != 0x80) return false;
            if ((byte4 & 0xC0) != 0x80) return false;
            const uint32_t codepoint = ((byte & 0x07) << 18) |
                                       ((byte2 & 0x3F) << 12) |
                                       ((byte3 & 0x3F) << 6) |
                                       (byte4 & 0x3F);
            if (codepoint < 0x10000 || codepoint > 0x10FFFF) return false;
            logical_index += 4;
            continue;
        }

        return false;
    }

    return true;
}

inline void wsApplyMaskIovecsInPlace(struct iovec* iovecs,
                                     size_t iovec_count,
                                     const uint8_t masking_key[4]) noexcept {
    size_t logical_index = 0;
    for (size_t i = 0; i < iovec_count; ++i) {
        auto* data = static_cast<char*>(iovecs[i].iov_base);
        for (size_t j = 0; j < iovecs[i].iov_len; ++j, ++logical_index) {
            data[j] ^= static_cast<char>(masking_key[logical_index % 4]);
        }
    }
}

inline WsFastPathPrefixResult scanWsMessageFastPathPrefix(const struct iovec* iovecs,
                                                          size_t iovec_count,
                                                          bool is_server) noexcept {
    WsFastPathPrefixResult result;
    const size_t total_length = wsIovecTotalLength(iovecs, iovec_count);
    if (total_length < 2) {
        result.status = WsMessageFastPathStatus::kNeedMore;
        return result;
    }

    uint8_t byte1 = 0;
    uint8_t byte2 = 0;
    if (!wsReadIovecBytes(iovecs, iovec_count, 0, &byte1, 1) ||
        !wsReadIovecBytes(iovecs, iovec_count, 1, &byte2, 1)) {
        result.status = WsMessageFastPathStatus::kNeedMore;
        return result;
    }

    if ((byte1 & 0x70) != 0) {
        return result;
    }

    const uint8_t opcode_value = byte1 & 0x0F;
    if (opcode_value > 0x0A || (opcode_value > 0x02 && opcode_value < 0x08)) {
        return result;
    }

    result.prefix.fin = (byte1 & 0x80) != 0;
    result.prefix.opcode = static_cast<WsOpcode>(opcode_value);
    if (isControlFrame(result.prefix.opcode)) {
        return result;
    }

    result.prefix.mask = (byte2 & 0x80) != 0;
    if ((is_server && !result.prefix.mask) || (!is_server && result.prefix.mask)) {
        return result;
    }

    uint64_t payload_length = byte2 & 0x7F;
    size_t header_length = 2;
    if (payload_length == 126) {
        if (total_length < header_length + 2) {
            result.status = WsMessageFastPathStatus::kNeedMore;
            return result;
        }
        uint8_t len_buf[2];
        if (!wsReadIovecBytes(iovecs, iovec_count, header_length, len_buf, sizeof(len_buf))) {
            result.status = WsMessageFastPathStatus::kNeedMore;
            return result;
        }
        payload_length = (static_cast<uint16_t>(len_buf[0]) << 8) | len_buf[1];
        header_length += sizeof(len_buf);
    } else if (payload_length == 127) {
        if (total_length < header_length + 8) {
            result.status = WsMessageFastPathStatus::kNeedMore;
            return result;
        }
        uint8_t len_buf[8];
        if (!wsReadIovecBytes(iovecs, iovec_count, header_length, len_buf, sizeof(len_buf))) {
            result.status = WsMessageFastPathStatus::kNeedMore;
            return result;
        }
        payload_length = 0;
        for (size_t i = 0; i < sizeof(len_buf); ++i) {
            payload_length = (payload_length << 8) | len_buf[i];
        }
        header_length += sizeof(len_buf);
    }

    if (payload_length > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
        return result;
    }

    if (result.prefix.mask) {
        if (total_length < header_length + sizeof(result.prefix.masking_key)) {
            result.status = WsMessageFastPathStatus::kNeedMore;
            return result;
        }
        if (!wsReadIovecBytes(iovecs,
                              iovec_count,
                              header_length,
                              result.prefix.masking_key,
                              sizeof(result.prefix.masking_key))) {
            result.status = WsMessageFastPathStatus::kNeedMore;
            return result;
        }
        header_length += sizeof(result.prefix.masking_key);
    }

    const uint64_t frame_length = header_length + payload_length;
    if (frame_length > static_cast<uint64_t>(total_length)) {
        result.status = WsMessageFastPathStatus::kNeedMore;
        return result;
    }

    result.prefix.payload_length = payload_length;
    result.prefix.payload_offset = header_length;
    result.prefix.frame_length = static_cast<size_t>(frame_length);
    result.status = WsMessageFastPathStatus::kContinue;
    return result;
}

inline bool bindWsConsumeFastPathView(const struct iovec* iovecs,
                                      size_t iovec_count,
                                      bool is_server,
                                      WsConsumeFastPathView& view) noexcept {
    const auto prefix_result = scanWsMessageFastPathPrefix(iovecs, iovec_count, is_server);
    if (prefix_result.status != WsMessageFastPathStatus::kContinue) {
        return false;
    }

    const auto& prefix = prefix_result.prefix;
    if (!prefix.fin || (prefix.opcode != WsOpcode::Text && prefix.opcode != WsOpcode::Binary)) {
        return false;
    }

    view.opcode = prefix.opcode;
    view.payload_length = static_cast<size_t>(prefix.payload_length);
    view.payload_offset = prefix.payload_offset;
    view.frame_length = prefix.frame_length;
    view.payload_data = nullptr;
    std::memcpy(view.masking_key, prefix.masking_key, sizeof(view.masking_key));
    view.payload_iovecs.setCount(0);

    size_t cursor = prefix.payload_offset;
    size_t remaining = view.payload_length;
    size_t out_count = 0;
    for (size_t i = 0; i < iovec_count && remaining > 0; ++i) {
        const auto& src = iovecs[i];
        if (cursor >= src.iov_len) {
            cursor -= src.iov_len;
            continue;
        }

        const size_t available = src.iov_len - cursor;
        const size_t take = std::min(available, remaining);
        view.payload_iovecs.storage()[out_count++] = {
            static_cast<char*>(src.iov_base) + cursor,
            take,
        };
        remaining -= take;
        cursor = 0;
        if (out_count == view.payload_iovecs.storage().size() && remaining > 0) {
            return false;
        }
    }

    if (remaining != 0) {
        return false;
    }
    view.payload_iovecs.setCount(out_count);
    if (out_count == 1) {
        view.payload_data = static_cast<char*>(view.payload_iovecs[0].iov_base);
    }
    return true;
}

template<typename StateT>
struct WsRingBufferTcpReadMachine {
    using result_type = typename StateT::ResultType;
    static constexpr auto kSequenceOwnerDomain = galay::kernel::SequenceOwnerDomain::Read;

    explicit WsRingBufferTcpReadMachine(std::shared_ptr<StateT> state)
        : m_state(std::move(state)) {}

    MachineAction<result_type> advance() {
        if (m_result.has_value()) {
            return MachineAction<result_type>::complete(std::move(*m_result));
        }

        if (m_state->parseFromBuffer()) {
            m_result = m_state->takeResult();
            return MachineAction<result_type>::complete(std::move(*m_result));
        }

        if (!m_state->prepareRecvWindow()) {
            m_result = m_state->takeResult();
            return MachineAction<result_type>::complete(std::move(*m_result));
        }

        return MachineAction<result_type>::waitReadv(
            m_state->recvIovecsData(),
            m_state->recvIovecsCount());
    }

    void onRead(std::expected<size_t, IOError> result) {
        if (!result) {
            m_state->setRecvError(result.error());
            m_result = m_state->takeResult();
            return;
        }

        if (result.value() == 0) {
            m_state->onPeerClosed();
            m_result = m_state->takeResult();
            return;
        }

        m_state->onBytesReceived(result.value());
    }

    void onWrite(std::expected<size_t, IOError>) {}

    std::shared_ptr<StateT> m_state;
    std::optional<result_type> m_result;
};

#ifdef GALAY_SSL_FEATURE_ENABLED
template<typename StateT>
struct WsRingBufferSslReadMachine {
    using result_type = typename StateT::ResultType;
    static constexpr auto kSequenceOwnerDomain = galay::kernel::SequenceOwnerDomain::Read;

    explicit WsRingBufferSslReadMachine(std::shared_ptr<StateT> state)
        : m_state(std::move(state)) {}

    galay::ssl::SslMachineAction<result_type> advance() {
        if (m_result.has_value()) {
            return galay::ssl::SslMachineAction<result_type>::complete(std::move(*m_result));
        }

        if (m_state->parseFromBuffer()) {
            m_result = m_state->takeResult();
            return galay::ssl::SslMachineAction<result_type>::complete(std::move(*m_result));
        }

        char* recv_buffer = nullptr;
        size_t recv_length = 0;
        if (!m_state->prepareRecvWindow(recv_buffer, recv_length)) {
            m_result = m_state->takeResult();
            return galay::ssl::SslMachineAction<result_type>::complete(std::move(*m_result));
        }

        return galay::ssl::SslMachineAction<result_type>::recv(recv_buffer, recv_length);
    }

    void onHandshake(std::expected<void, galay::ssl::SslError>) {}

    void onRecv(std::expected<Bytes, galay::ssl::SslError> result) {
        if (!result) {
            m_state->setSslRecvError(result.error());
            m_result = m_state->takeResult();
            return;
        }

        const size_t recv_bytes = result.value().size();
        if (recv_bytes == 0) {
            m_state->onPeerClosed();
            m_result = m_state->takeResult();
            return;
        }

        m_state->onBytesReceived(recv_bytes);
    }

    void onSend(std::expected<size_t, galay::ssl::SslError>) {}

    void onShutdown(std::expected<void, galay::ssl::SslError>) {}

    std::shared_ptr<StateT> m_state;
    std::optional<result_type> m_result;
};
#endif

struct WsFrameReadState {
    using ResultType = std::expected<bool, WsError>;

    WsFrameReadState(RingBuffer& ring_buffer,
                     const WsReaderSetting& setting,
                     WsFrame& frame,
                     bool is_server)
        : m_ring_buffer(&ring_buffer)
        , m_setting(setting)
        , m_frame(&frame)
        , m_is_server(is_server) {}

    bool parseFromBuffer() {
        auto read_iovecs = borrowReadIovecs(*m_ring_buffer);
        if (read_iovecs.empty()) {
            return false;
        }

        auto parse_result = WsFrameParser::fromIOVec(
            read_iovecs.data(),
            read_iovecs.size(),
            *m_frame,
            m_is_server);
        if (!parse_result.has_value()) {
            WsError error = parse_result.error();
                if (error.code() == kWsIncomplete) {
                    const size_t buffered = m_ring_buffer->readable();
                    if (m_total_received + buffered > m_setting.max_frame_size) {
                        setParseError(WsError(kWsMessageTooLarge, "Frame size exceeds limit"));
                        return true;
                    }
                    return false;
                }
            setParseError(std::move(error));
            return true;
        }

        m_ring_buffer->consume(parse_result.value());
        if (m_frame->header.payload_length > m_setting.max_frame_size) {
            setParseError(WsError(kWsMessageTooLarge, "Frame payload too large"));
            return true;
        }
        return true;
    }

    bool prepareRecvWindow() {
        m_write_iovecs = borrowWriteIovecs(*m_ring_buffer);
        if (m_write_iovecs.empty()) {
            setParseError(WsError(kWsConnectionError, "Ring buffer has no space for writing"));
            return false;
        }
        return true;
    }

    bool prepareRecvWindow(char*& buffer, size_t& length) {
        if (!prepareRecvWindow()) {
            buffer = nullptr;
            length = 0;
            return false;
        }
        if (!IoVecWindow::bindFirstNonEmpty(m_write_iovecs, buffer, length)) {
            setParseError(WsError(kWsConnectionError, "Ring buffer has no space for writing"));
            buffer = nullptr;
            length = 0;
            return false;
        }
        m_recv_staged = false;
        return length > 0;
    }

    const struct iovec* recvIovecsData() const { return m_write_iovecs.data(); }
    size_t recvIovecsCount() const { return m_write_iovecs.size(); }

    void setRecvError(const IOError& io_error) {
        if (IOError::contains(io_error.code(), kDisconnectError)) {
            m_ws_error = WsError(kWsConnectionClosed, io_error.message());
            return;
        }
        m_ws_error = WsError(kWsConnectionError, io_error.message());
    }

#ifdef GALAY_SSL_FEATURE_ENABLED
    void setSslRecvError(const galay::ssl::SslError& error) {
        if (error.code() == galay::ssl::SslErrorCode::kPeerClosed) {
            m_ws_error = WsError(kWsConnectionClosed, "Connection closed by peer");
            return;
        }
        m_ws_error = WsError(kWsConnectionError, error.message());
    }
#endif

    void onPeerClosed() {
        m_ws_error = WsError(kWsConnectionClosed, "Connection closed by peer");
    }

    void onBytesReceived(size_t recv_bytes) {
        m_total_received += recv_bytes;
        if (m_recv_staged) {
            size_t copied = 0;
            for (const auto& iov : m_write_iovecs) {
                if (copied >= recv_bytes) {
                    break;
                }
                const size_t to_copy = std::min(iov.iov_len, recv_bytes - copied);
                std::memcpy(iov.iov_base, m_ssl_recv_scratch.data() + copied, to_copy);
                copied += to_copy;
            }
        }
        m_ring_buffer->produce(recv_bytes);
        m_recv_staged = false;
    }

    void setParseError(WsError&& error) { m_ws_error = std::move(error); }

    ResultType takeResult() {
        if (m_ws_error.has_value()) {
            return std::unexpected(std::move(*m_ws_error));
        }
        return true;
    }

    RingBuffer* m_ring_buffer;
    WsReaderSetting m_setting;
    WsFrame* m_frame;
    bool m_is_server;
    size_t m_total_received = 0;
    BorrowedIovecs<2> m_write_iovecs;
    std::vector<char> m_ssl_recv_scratch;
    bool m_recv_staged = false;
    std::optional<WsError> m_ws_error;
};

struct WsMessageReadState {
    using ResultType = std::expected<bool, WsError>;

    WsMessageReadState(RingBuffer& ring_buffer,
                       const WsReaderSetting& setting,
                       std::string& message,
                       WsOpcode& opcode,
                       bool is_server,
                       bool use_mask,
                       ControlFrameCallback control_frame_callback,
                       bool enable_fast_path = true)
        : m_ring_buffer(&ring_buffer)
        , m_setting(setting)
        , m_message(&message)
        , m_opcode(&opcode)
        , m_is_server(is_server)
        , m_use_mask(use_mask)
        , m_control_frame_callback(std::move(control_frame_callback))
        , m_enable_fast_path(enable_fast_path) {}

    bool parseFromBuffer() {
        while (true) {
            auto read_iovecs = borrowReadIovecs(*m_ring_buffer);
            if (read_iovecs.empty()) {
                return false;
            }

            if (m_enable_fast_path) {
                switch (tryFastPath(read_iovecs.data(), read_iovecs.size())) {
                    case WsMessageFastPathStatus::kReturn:
                        return true;
                    case WsMessageFastPathStatus::kContinue:
                        if (m_ring_buffer->readable() == 0) {
                            return false;
                        }
                        continue;
                    case WsMessageFastPathStatus::kNeedMore: {
                        const size_t buffered = m_ring_buffer->readable();
                        if (m_message->size() + m_total_received + buffered > m_setting.max_message_size) {
                            setParseError(WsError(kWsMessageTooLarge, "Message size exceeds limit"));
                            return true;
                        }
                        return false;
                    }
                    case WsMessageFastPathStatus::kFallback:
                        break;
                }
            }

            WsFrame frame;
            auto parse_result = WsFrameParser::fromIOVec(
                read_iovecs.data(),
                read_iovecs.size(),
                frame,
                m_is_server);
            if (!parse_result.has_value()) {
                WsError error = parse_result.error();
                if (error.code() == kWsIncomplete) {
                    const size_t buffered = m_ring_buffer->readable();
                    if (m_message->size() + m_total_received + buffered > m_setting.max_message_size) {
                        setParseError(WsError(kWsMessageTooLarge, "Message size exceeds limit"));
                        return true;
                    }
                    return false;
                }
                setParseError(std::move(error));
                return true;
            }

            m_ring_buffer->consume(parse_result.value());

            if (isControlFrame(frame.header.opcode)) {
                if (!frame.header.fin) {
                    setParseError(WsError(kWsControlFrameFragmented));
                    return true;
                }
                if (m_control_frame_callback) {
                    m_control_frame_callback(frame.header.opcode, frame.payload);
                }
                *m_message = std::move(frame.payload);
                *m_opcode = frame.header.opcode;
                return true;
            }

            if (m_first_frame) {
                if (frame.header.opcode == WsOpcode::Continuation) {
                    setParseError(WsError(kWsProtocolError, "First frame cannot be continuation"));
                    return true;
                }
                *m_opcode = frame.header.opcode;
                m_first_frame = false;
                *m_message = std::move(frame.payload);
            } else {
                if (frame.header.opcode != WsOpcode::Continuation) {
                    setParseError(WsError(kWsProtocolError, "Expected continuation frame"));
                    return true;
                }
                m_message->append(frame.payload);
            }

            if (m_message->size() > m_setting.max_message_size) {
                setParseError(WsError(kWsMessageTooLarge, "Message size exceeds limit"));
                return true;
            }

            if (frame.header.fin) {
                return true;
            }

            if (m_ring_buffer->readable() == 0) {
                return false;
            }
        }
    }

    WsMessageFastPathStatus tryFastPath(const struct iovec* iovecs, size_t iovec_count) {
        const auto prefix_result = scanWsMessageFastPathPrefix(iovecs, iovec_count, m_is_server);
        if (prefix_result.status != WsMessageFastPathStatus::kContinue) {
            return prefix_result.status;
        }

        const auto& prefix = prefix_result.prefix;
        if (m_first_frame) {
            if (prefix.opcode == WsOpcode::Continuation) {
                m_ring_buffer->consume(prefix.frame_length);
                setParseError(WsError(kWsProtocolError, "First frame cannot be continuation"));
                return WsMessageFastPathStatus::kReturn;
            }
        } else if (prefix.opcode != WsOpcode::Continuation) {
            m_ring_buffer->consume(prefix.frame_length);
            setParseError(WsError(kWsProtocolError, "Expected continuation frame"));
            return WsMessageFastPathStatus::kReturn;
        }

        const size_t payload_size = static_cast<size_t>(prefix.payload_length);
        const size_t old_size = m_message->size();
        const bool replace_message = m_first_frame;
        const size_t write_offset = old_size;

        m_message->resize(old_size + payload_size);
        if (payload_size > 0 &&
            !wsReadIovecBytes(iovecs,
                              iovec_count,
                              prefix.payload_offset,
                              m_message->data() + write_offset,
                              payload_size)) {
            m_message->resize(old_size);
            return WsMessageFastPathStatus::kFallback;
        }

        if (prefix.mask) {
            wsApplyMaskInPlace(m_message->data() + write_offset, payload_size, prefix.masking_key);
        }

        if (prefix.opcode == WsOpcode::Text &&
            prefix.fin &&
            !wsIsValidUtf8Span(m_message->data() + write_offset, payload_size)) {
            m_message->resize(old_size);
            setParseError(WsError(kWsInvalidUtf8));
            return WsMessageFastPathStatus::kReturn;
        }

        if (replace_message) {
            if (old_size != 0 && payload_size != 0) {
                std::memmove(m_message->data(), m_message->data() + old_size, payload_size);
            }
            m_message->resize(payload_size);
            *m_opcode = prefix.opcode;
            m_first_frame = false;
        }

        m_ring_buffer->consume(prefix.frame_length);
        ++m_fast_path_frames;

        if (m_message->size() > m_setting.max_message_size) {
            setParseError(WsError(kWsMessageTooLarge, "Message size exceeds limit"));
            return WsMessageFastPathStatus::kReturn;
        }

        if (prefix.fin) {
            return WsMessageFastPathStatus::kReturn;
        }

        return WsMessageFastPathStatus::kContinue;
    }

    bool prepareRecvWindow() {
        m_write_iovecs = borrowWriteIovecs(*m_ring_buffer);
        if (m_write_iovecs.empty()) {
            setParseError(WsError(kWsConnectionError, "Ring buffer has no space for writing"));
            return false;
        }
        return true;
    }

    bool prepareRecvWindow(char*& buffer, size_t& length) {
        if (!prepareRecvWindow()) {
            buffer = nullptr;
            length = 0;
            return false;
        }
        if (!IoVecWindow::bindFirstNonEmpty(m_write_iovecs, buffer, length)) {
            setParseError(WsError(kWsConnectionError, "Ring buffer has no space for writing"));
            buffer = nullptr;
            length = 0;
            return false;
        }
        m_recv_staged = false;
        return length > 0;
    }

    const struct iovec* recvIovecsData() const { return m_write_iovecs.data(); }
    size_t recvIovecsCount() const { return m_write_iovecs.size(); }

    void setRecvError(const IOError& io_error) {
        if (IOError::contains(io_error.code(), kDisconnectError)) {
            m_ws_error = WsError(kWsConnectionClosed, io_error.message());
            return;
        }
        m_ws_error = WsError(kWsConnectionError, io_error.message());
    }

#ifdef GALAY_SSL_FEATURE_ENABLED
    void setSslRecvError(const galay::ssl::SslError& error) {
        if (error.code() == galay::ssl::SslErrorCode::kPeerClosed) {
            m_ws_error = WsError(kWsConnectionClosed, "Connection closed by peer");
            return;
        }
        m_ws_error = WsError(kWsConnectionError, error.message());
    }
#endif

    void onPeerClosed() {
        m_ws_error = WsError(kWsConnectionClosed, "Connection closed by peer");
    }

    void onBytesReceived(size_t recv_bytes) {
        m_total_received += recv_bytes;
        if (m_recv_staged) {
            size_t copied = 0;
            for (const auto& iov : m_write_iovecs) {
                if (copied >= recv_bytes) {
                    break;
                }
                const size_t to_copy = std::min(iov.iov_len, recv_bytes - copied);
                std::memcpy(iov.iov_base, m_ssl_recv_scratch.data() + copied, to_copy);
                copied += to_copy;
            }
        }
        m_ring_buffer->produce(recv_bytes);
        m_recv_staged = false;
    }

    void setParseError(WsError&& error) { m_ws_error = std::move(error); }

    void resetForNextMessage() {
        m_total_received = 0;
        m_first_frame = true;
        m_fast_path_frames = 0;
        m_recv_staged = false;
        m_ws_error.reset();
        m_write_iovecs.setCount(0);
        if (m_message != nullptr) {
            m_message->clear();
        }
    }

    ResultType takeResult() {
        if (m_ws_error.has_value()) {
            return std::unexpected(std::move(*m_ws_error));
        }
        return true;
    }

    RingBuffer* m_ring_buffer;
    WsReaderSetting m_setting;
    std::string* m_message;
    WsOpcode* m_opcode;
    bool m_is_server;
    bool m_use_mask;
    size_t m_total_received = 0;
    bool m_first_frame = true;
    size_t m_fast_path_frames = 0;
    ControlFrameCallback m_control_frame_callback;
    bool m_enable_fast_path = true;
    BorrowedIovecs<2> m_write_iovecs;
    std::vector<char> m_ssl_recv_scratch;
    bool m_recv_staged = false;
    std::optional<WsError> m_ws_error;
};

template<typename SocketType, typename StateT>
auto buildReadOperation(SocketType& socket, std::shared_ptr<StateT> state) {
    using ResultType = typename StateT::ResultType;
    if constexpr (is_ssl_socket_v<SocketType>) {
#ifdef GALAY_SSL_FEATURE_ENABLED
        return galay::ssl::SslAwaitableBuilder<ResultType>::fromStateMachine(
                   socket.controller(),
                   &socket,
                   WsRingBufferSslReadMachine<StateT>(std::move(state)))
            .build();
#else
        static_assert(!sizeof(SocketType), "SSL support is disabled");
#endif
    } else {
        return AwaitableBuilder<ResultType>::fromStateMachine(
                   socket.controller(),
                   WsRingBufferTcpReadMachine<StateT>(std::move(state)))
            .build();
    }
}

} // namespace detail

/**
 * @brief WebSocket 读取器模板类
 * @tparam SocketType Socket 类型（TcpSocket 或 SslSocket）
 * @details 从 Socket 异步读取 WebSocket 帧和消息，
 *          支持快速路径优化、UTF-8 校验、掩码解掩和分片消息重组。
 */
template<typename SocketType>
class WsReaderImpl {
public:
    struct OperationCounters {
        size_t frame_awaitables_started = 0;    ///< 帧读取操作启动次数
        size_t message_awaitables_started = 0;  ///< 消息读取操作启动次数
    };

    /**
     * @brief 构造函数
     * @param ring_buffer 环形缓冲区引用
     * @param setting 读取器配置
     * @param socket Socket 引用
     * @param is_server 是否为服务器端
     * @param use_mask 是否使用掩码
     */
    WsReaderImpl(RingBuffer& ring_buffer,
                 const WsReaderSetting& setting,
                 SocketType& socket,
                 bool is_server = true,
                 bool use_mask = false)
        : m_ring_buffer(&ring_buffer)
        , m_setting(setting)
        , m_socket(&socket)
        , m_is_server(is_server)
        , m_use_mask(use_mask) {}

    /**
     * @brief 异步读取一个 WebSocket 帧
     * @param frame 待填充的帧对象
     * @return 可 co_await 的异步操作
     */
    auto getFrame(WsFrame& frame) {
        ++m_operation_counters.frame_awaitables_started;
        return detail::buildReadOperation(
            *m_socket,
            std::make_shared<detail::WsFrameReadState>(*m_ring_buffer, m_setting, frame, m_is_server));
    }

    /**
     * @brief 异步读取一个完整的 WebSocket 消息（自动重组分片）
     * @param message 输出消息内容
     * @param opcode 输出消息操作码
     * @return 可 co_await 的异步操作
     */
    auto getMessage(std::string& message, WsOpcode& opcode) {
        ++m_operation_counters.message_awaitables_started;
        return detail::buildReadOperation(
            *m_socket,
            std::make_shared<detail::WsMessageReadState>(
                *m_ring_buffer,
                m_setting,
                message,
                opcode,
                m_is_server,
                m_use_mask,
                nullptr,
                messageFastPathEnabled()));
    }

private:
    static constexpr bool messageFastPathEnabled() noexcept {
        return true;
    }

    RingBuffer* m_ring_buffer;
    WsReaderSetting m_setting;
    SocketType* m_socket;
    bool m_is_server;
    bool m_use_mask;
    OperationCounters m_operation_counters;
};

using WsReader = WsReaderImpl<TcpSocket>;

} // namespace galay::websocket

#ifdef GALAY_SSL_FEATURE_ENABLED
namespace galay::websocket {
using WssReader = WsReaderImpl<galay::ssl::SslSocket>;
} // namespace galay::websocket
#endif

#endif // GALAY_WS_READER_H
