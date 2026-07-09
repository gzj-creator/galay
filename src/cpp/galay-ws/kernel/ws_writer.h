/**
 * @file ws_writer.h
 * @brief WebSocket 写入器，基于异步状态机将 WebSocket 帧写入 Socket
 * @author galay-http
 * @version 1.0.0
 *
 * @details 提供 WsWriterImpl 模板类，支持将文本、二进制和控制帧
 *          异步写入 TcpSocket 或 SslSocket。TCP 模式使用 writev 零拷贝，
 *          SSL 模式使用 send。内部实现快速路径优化常用帧的发送。
 */

#ifndef GALAY_WS_WRITER_H
#define GALAY_WS_WRITER_H

#include "writer_cfg.h"
#include "../../galay-http/common/iovec_utils.h"
#include "../protoc/ws_frame.h"
#include "../protoc/ws_error.h"
#include "../../galay-kernel/core/awaitable.h"
#include "../../galay-kernel/async/tcp_socket.h"
#include <array>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <sys/uio.h>

#ifdef GALAY_SSL_FEATURE_ENABLED
#include "../../galay-utils/cache/bytes.hpp"
#include "../../galay-ssl/async/ssl_await.h"
#include "../../galay-ssl/async/ssl_socket.h"
#endif

namespace galay::websocket
{

using namespace galay::kernel;
using namespace galay::async;
#ifdef GALAY_SSL_FEATURE_ENABLED
using ::galay::utils::Bytes;
#endif

template<typename SocketType>
class WsWriterImpl;

template<typename T>
struct is_tcp_socket : std::false_type {};

template<>
struct is_tcp_socket<TcpSocket> : std::true_type {};

template<typename T>
inline constexpr bool is_tcp_socket_v = is_tcp_socket<T>::value;

template<typename T>
struct is_ws_writer_ssl_socket : std::false_type {};

#ifdef GALAY_SSL_FEATURE_ENABLED
template<>
struct is_ws_writer_ssl_socket<galay::ssl::SslSocket> : std::true_type {};
#endif

template<typename T>
inline constexpr bool is_ws_writer_ssl_socket_v = is_ws_writer_ssl_socket<T>::value;

namespace detail {

template<typename SocketType>
struct WsEchoMachine;

template<typename SocketType>
struct WsSslEchoMachine;

template<typename SocketType>
struct WsTcpWritevMachine {
    using result_type = std::expected<bool, WsError>;
    static constexpr auto kSequenceOwnerDomain = galay::kernel::SequenceOwnerDomain::Write;

    explicit WsTcpWritevMachine(WsWriterImpl<SocketType>* writer)
        : m_writer(writer) {}

private:
    WsTcpWritevMachine(const WsTcpWritevMachine&) = delete;
    WsTcpWritevMachine& operator=(const WsTcpWritevMachine&) = delete;
public:
    WsTcpWritevMachine(WsTcpWritevMachine&&) noexcept = default;
    WsTcpWritevMachine& operator=(WsTcpWritevMachine&&) noexcept = default;

    MachineAction<result_type> advance() {
        if (m_result.has_value()) {
            return MachineAction<result_type>::complete(std::move(*m_result));
        }

        if (m_writer->getRemainingBytes() == 0) {
            m_result = true;
            return MachineAction<result_type>::complete(true);
        }

        const auto* iov_data = m_writer->getIovecsData();
        const auto iov_count = m_writer->getIovecsCount();
        if (iov_data == nullptr || iov_count == 0) {
            failWithMessage("No remaining iovec to write");
            return MachineAction<result_type>::complete(std::move(*m_result));
        }

        return MachineAction<result_type>::waitWritev(iov_data, iov_count);
    }

    void onRead(std::expected<size_t, IOError>) {}

    void onWrite(std::expected<size_t, IOError> result) {
        if (!result) {
            m_result = std::unexpected(WsError(kWsSendError, result.error().message()));
            return;
        }

        const size_t written = result.value();
        if (written > 0) {
            m_writer->updateRemainingWritev(written);
        }

        if (m_writer->getRemainingBytes() == 0) {
            m_result = true;
            return;
        }

        if (m_writer->getIovecsData() == nullptr || m_writer->getIovecsCount() == 0) {
            failWithMessage("No remaining iovec to write");
        }
    }

private:
    void failWithMessage(const char* message) {
        m_result = std::unexpected(WsError(kWsSendError, message));
    }

    WsWriterImpl<SocketType>* m_writer;
    std::optional<result_type> m_result;
};

#ifdef GALAY_SSL_FEATURE_ENABLED
template<typename SocketType>
struct WsSslSendMachine {
    using result_type = std::expected<bool, WsError>;
    static constexpr auto kSequenceOwnerDomain = galay::kernel::SequenceOwnerDomain::Write;

    explicit WsSslSendMachine(WsWriterImpl<SocketType>* writer)
        : m_writer(writer) {}

private:
    WsSslSendMachine(const WsSslSendMachine&) = delete;
    WsSslSendMachine& operator=(const WsSslSendMachine&) = delete;
public:
    WsSslSendMachine(WsSslSendMachine&&) noexcept = default;
    WsSslSendMachine& operator=(WsSslSendMachine&&) noexcept = default;

    galay::ssl::SslMachineAction<result_type> advance() {
        if (m_result.has_value()) {
            return galay::ssl::SslMachineAction<result_type>::complete(std::move(*m_result));
        }

        if (m_writer->getRemainingBytes() == 0) {
            m_result = true;
            return galay::ssl::SslMachineAction<result_type>::complete(true);
        }

        return galay::ssl::SslMachineAction<result_type>::send(
            m_writer->bufferData() + m_writer->sentBytes(),
            m_writer->getRemainingBytes());
    }

    void onHandshake(std::expected<void, galay::ssl::SslError>) {}
    void onRecv(std::expected<Bytes, galay::ssl::SslError>) {}
    void onShutdown(std::expected<void, galay::ssl::SslError>) {}

    void onSend(std::expected<size_t, galay::ssl::SslError> result) {
        if (!result) {
            m_writer->resetPendingState();
            m_result = std::unexpected(WsError(kWsSendError, result.error().message()));
            return;
        }

        if (result.value() == 0) {
            m_writer->resetPendingState();
            m_result = std::unexpected(WsError(kWsSendError, "SSL send returned zero bytes"));
            return;
        }

        m_writer->updateRemaining(result.value());
        if (m_writer->getRemainingBytes() == 0) {
            m_result = true;
        }
    }

private:
    WsWriterImpl<SocketType>* m_writer;
    std::optional<result_type> m_result;
};
#endif

template<typename SocketType>
auto buildSendAwaitable(SocketType& socket, WsWriterImpl<SocketType>& writer) {
    using ResultType = std::expected<bool, WsError>;
    if constexpr (is_ws_writer_ssl_socket_v<SocketType>) {
#ifdef GALAY_SSL_FEATURE_ENABLED
        return galay::ssl::SslAwaitableBuilder<ResultType>::fromStateMachine(
                   socket.controller(),
                   &socket,
                   WsSslSendMachine<SocketType>(&writer))
            .build();
#else
        static_assert(!sizeof(SocketType), "SSL support is disabled");
#endif
    } else {
        return AwaitableBuilder<ResultType>::fromStateMachine(
                   socket.controller(),
                   WsTcpWritevMachine<SocketType>(&writer))
            .build();
    }
}

} // namespace detail

/**
 * @brief WebSocket 写入器模板类
 * @tparam SocketType Socket 类型（TcpSocket 或 SslSocket）
 * @details 将 WebSocket 帧（文本、二进制、控制帧）异步写入 Socket。
 *          TCP 模式使用 writev 零拷贝，SSL 模式使用 send。
 *          内部实现快速路径优化常用帧的序列化。
 */
template<typename SocketType>
class WsWriterImpl
{
public:
    struct OperationCounters {
        size_t send_awaitables_started = 0; ///< 发送操作启动次数
    };

    struct FastPathCounters {
        size_t hits = 0;     ///< 快速路径命中次数
        size_t fallbacks = 0; ///< 回退路径次数
    };

    /**
     * @brief 构造函数
     * @param setting 写入器配置
     * @param socket Socket 引用
     */
    WsWriterImpl(const WsWriterSetting& setting, SocketType& socket)
        : m_setting(setting)
        , m_socket(&socket)
        , m_remaining_bytes(0)
    {
        m_writev_cursor.reserve(2);
    }

private:
    WsWriterImpl(const WsWriterImpl&) = delete;
    WsWriterImpl& operator=(const WsWriterImpl&) = delete;
public:

    /**
     * @brief 移动构造，保留 pending 发送进度
     * @details TCP writev 游标保存的是指向本对象缓冲区的 iovec，移动后会
     *          重新绑定到新对象的 header/payload 存储。
     */
    WsWriterImpl(WsWriterImpl&& other) noexcept
        : m_setting(other.m_setting)
        , m_socket(other.m_socket)
        , m_remaining_bytes(0)
    {
        moveFrom(std::move(other));
    }

    /**
     * @brief 移动赋值，保留 pending 发送进度
     * @details 与移动构造一样会重新绑定 TCP writev 游标到当前对象。
     */
    WsWriterImpl& operator=(WsWriterImpl&& other) noexcept {
        if (this == &other) {
            return *this;
        }
        moveFrom(std::move(other));
        return *this;
    }

    auto sendText(const std::string& text, bool fin = true) {
        if (m_remaining_bytes == 0) {
            ++m_operation_counters.send_awaitables_started;
            if constexpr (!is_tcp_socket_v<SocketType>) {
                prepareSslMessage(WsOpcode::Text, text, fin);
            } else if (!tryPrepareCommonTcpFrame(WsOpcode::Text, text, fin)) {
                WsFrame frame = WsFrameParser::createTextFrame(text, fin);
                prepareSendFrame(std::move(frame));
            }
        }
        return makeSendAwaitable();
    }

    auto sendText(std::string&& text, bool fin = true) {
        if (m_remaining_bytes == 0) {
            ++m_operation_counters.send_awaitables_started;
            if constexpr (!is_tcp_socket_v<SocketType>) {
                prepareSslMessage(WsOpcode::Text, std::move(text), fin);
            } else if (!tryPrepareCommonTcpFrame(WsOpcode::Text, std::move(text), fin)) {
                WsFrame frame = WsFrameBuilder().text(std::move(text), fin).buildMove();
                prepareSendFrame(std::move(frame));
            }
        }
        return makeSendAwaitable();
    }

    auto sendBinary(const std::string& data, bool fin = true) {
        if (m_remaining_bytes == 0) {
            ++m_operation_counters.send_awaitables_started;
            if constexpr (!is_tcp_socket_v<SocketType>) {
                prepareSslMessage(WsOpcode::Binary, data, fin);
            } else if (!tryPrepareCommonTcpFrame(WsOpcode::Binary, data, fin)) {
                WsFrame frame = WsFrameParser::createBinaryFrame(data, fin);
                prepareSendFrame(std::move(frame));
            }
        }
        return makeSendAwaitable();
    }

    auto sendBinary(std::string&& data, bool fin = true) {
        if (m_remaining_bytes == 0) {
            ++m_operation_counters.send_awaitables_started;
            if constexpr (!is_tcp_socket_v<SocketType>) {
                prepareSslMessage(WsOpcode::Binary, std::move(data), fin);
            } else if (!tryPrepareCommonTcpFrame(WsOpcode::Binary, std::move(data), fin)) {
                WsFrame frame = WsFrameBuilder().binary(std::move(data), fin).buildMove();
                prepareSendFrame(std::move(frame));
            }
        }
        return makeSendAwaitable();
    }

    auto sendPing(const std::string& data = "") {
        if (m_remaining_bytes == 0) {
            ++m_operation_counters.send_awaitables_started;
            WsFrame frame = WsFrameParser::createPingFrame(data);
            prepareSendFrame(std::move(frame));
        }
        return makeSendAwaitable();
    }

    auto sendPong(const std::string& data = "") {
        if (m_remaining_bytes == 0) {
            ++m_operation_counters.send_awaitables_started;
            WsFrame frame = WsFrameParser::createPongFrame(data);
            prepareSendFrame(std::move(frame));
        }
        return makeSendAwaitable();
    }

    auto sendClose(WsCloseCode code = WsCloseCode::Normal, const std::string& reason = "") {
        if (m_remaining_bytes == 0) {
            ++m_operation_counters.send_awaitables_started;
            WsFrame frame = WsFrameParser::createCloseFrame(code, reason);
            prepareSendFrame(std::move(frame));
        }
        return makeSendAwaitable();
    }

    auto sendFrame(const WsFrame& frame) {
        if (m_remaining_bytes == 0) {
            ++m_operation_counters.send_awaitables_started;
            prepareSendFrame(frame);
        }
        return makeSendAwaitable();
    }

    auto sendFrame(WsFrame&& frame) {
        if (m_remaining_bytes == 0) {
            ++m_operation_counters.send_awaitables_started;
            prepareSendFrame(std::move(frame));
        }
        return makeSendAwaitable();
    }

    void prepareSslMessage(WsOpcode opcode, std::string_view payload, bool fin = true) {
        resetPendingState();
        WsFrameParser::encodeMessageInto(m_buffer, opcode, payload, fin, m_setting.use_mask);
        m_remaining_bytes = m_buffer.size();
    }

    void prepareSslMessage(WsOpcode opcode, std::string&& payload, bool fin = true) {
        resetPendingState();
        WsFrameParser::encodeMessageInto(m_buffer, opcode, std::move(payload), fin, m_setting.use_mask);
        m_remaining_bytes = m_buffer.size();
    }

private:
    enum class PendingWritevBuffer : uint8_t {
        kHeader,
        kPayload,
    };

    struct PendingWritevSegment {
        size_t offset = 0;
        size_t length = 0;
        PendingWritevBuffer buffer = PendingWritevBuffer::kHeader;
    };

    struct PendingWritevSnapshot {
        std::array<PendingWritevSegment, 2> segments{};
        size_t count = 0;
    };

    auto makeSendAwaitable() {
        return detail::buildSendAwaitable(*m_socket, *this);
    }

    static bool capturePendingSegment(const struct iovec& segment,
                                      const std::string& buffer,
                                      PendingWritevBuffer buffer_kind,
                                      PendingWritevSnapshot& snapshot) noexcept {
        if (buffer.empty() || segment.iov_base == nullptr || segment.iov_len == 0) {
            return false;
        }

        const auto begin = reinterpret_cast<std::uintptr_t>(buffer.data());
        const auto current = reinterpret_cast<std::uintptr_t>(segment.iov_base);
        const auto end = begin + buffer.size();
        if (current < begin || current >= end) {
            return false;
        }

        const size_t offset = static_cast<size_t>(current - begin);
        if (segment.iov_len > buffer.size() - offset ||
            snapshot.count >= snapshot.segments.size()) {
            return false;
        }

        snapshot.segments[snapshot.count++] = PendingWritevSegment{
            .offset = offset,
            .length = segment.iov_len,
            .buffer = buffer_kind,
        };
        return true;
    }

    static PendingWritevSnapshot snapshotPendingWritev(const WsWriterImpl& writer) noexcept {
        PendingWritevSnapshot snapshot;
        const struct iovec* iovecs = writer.m_writev_cursor.data();
        const size_t iovec_count = writer.m_writev_cursor.count();
        for (size_t i = 0; i < iovec_count && snapshot.count < snapshot.segments.size(); ++i) {
            if (capturePendingSegment(
                    iovecs[i],
                    writer.m_buffer,
                    PendingWritevBuffer::kHeader,
                    snapshot)) {
                continue;
            }
            const bool captured_payload = capturePendingSegment(
                iovecs[i],
                writer.m_payload_buffer,
                PendingWritevBuffer::kPayload,
                snapshot);
            if (!captured_payload) {
                return snapshot;
            }
        }
        return snapshot;
    }

    void restorePendingWritev(const PendingWritevSnapshot& snapshot) noexcept {
        if (snapshot.count == 0) {
            return;
        }

        m_writev_cursor.clear();
        for (size_t i = 0; i < snapshot.count; ++i) {
            const auto& segment = snapshot.segments[i];
            std::string& buffer =
                segment.buffer == PendingWritevBuffer::kHeader ? m_buffer : m_payload_buffer;
            m_writev_cursor.append({
                buffer.data() + segment.offset,
                segment.length,
            });
        }
        m_remaining_bytes = m_writev_cursor.remainingBytes();
    }

    void moveFrom(WsWriterImpl&& other) noexcept {
        const PendingWritevSnapshot pending_writev = snapshotPendingWritev(other);

        m_setting = other.m_setting;
        m_socket = other.m_socket;
        m_buffer = std::move(other.m_buffer);
        m_payload_buffer = std::move(other.m_payload_buffer);
        m_writev_cursor = std::move(other.m_writev_cursor);
        m_remaining_bytes = other.m_remaining_bytes;
        m_operation_counters = other.m_operation_counters;
        m_fast_path_counters = other.m_fast_path_counters;
        for (size_t i = 0; i < sizeof(m_masking_key); ++i) {
            m_masking_key[i] = other.m_masking_key[i];
        }

        restorePendingWritev(pending_writev);
        other.resetPendingState();
    }

    static constexpr bool canUseCommonTcpFastPath(WsOpcode opcode, bool fin, bool use_mask) {
        return !use_mask &&
               fin &&
               (opcode == WsOpcode::Text || opcode == WsOpcode::Binary);
    }

    bool tryPrepareCommonTcpFrame(WsOpcode opcode, const std::string& payload, bool fin) {
        if constexpr (!is_tcp_socket_v<SocketType>) {
            return false;
        } else {
            if (!canUseCommonTcpFastPath(opcode, fin, m_setting.use_mask)) {
                return false;
            }

            prepareCommonTcpFrameHeader(opcode, payload.size());
            m_payload_buffer = payload;
            finalizeWritevBuffers(true);
            return true;
        }
    }

    bool tryPrepareCommonTcpFrame(WsOpcode opcode, std::string&& payload, bool fin) {
        if constexpr (!is_tcp_socket_v<SocketType>) {
            return false;
        } else {
            if (!canUseCommonTcpFastPath(opcode, fin, m_setting.use_mask)) {
                return false;
            }

            prepareCommonTcpFrameHeader(opcode, payload.size());
            m_payload_buffer = std::move(payload);
            finalizeWritevBuffers(true);
            return true;
        }
    }

    bool tryPrepareCommonTcpFrame(const WsFrame& frame) {
        if constexpr (!is_tcp_socket_v<SocketType>) {
            return false;
        } else {
            if (!canUseCommonTcpFastPath(frame.header.opcode, frame.header.fin, m_setting.use_mask)) {
                return false;
            }

            prepareCommonTcpFrameHeader(frame.header.opcode, frame.payload.size());
            m_payload_buffer = frame.payload;
            finalizeWritevBuffers(true);
            return true;
        }
    }

    bool tryPrepareCommonTcpFrame(WsFrame&& frame) {
        if constexpr (!is_tcp_socket_v<SocketType>) {
            return false;
        } else {
            if (!canUseCommonTcpFastPath(frame.header.opcode, frame.header.fin, m_setting.use_mask)) {
                return false;
            }

            prepareCommonTcpFrameHeader(frame.header.opcode, frame.payload.size());
            m_payload_buffer = std::move(frame.payload);
            finalizeWritevBuffers(true);
            return true;
        }
    }

    void prepareCommonTcpFrameHeader(WsOpcode opcode, size_t payload_size) {
        m_buffer.clear();
        const uint64_t payload_len = static_cast<uint64_t>(payload_size);
        if (payload_len < 126) {
            m_buffer.reserve(2);
        } else if (payload_len <= 0xFFFF) {
            m_buffer.reserve(4);
        } else {
            m_buffer.reserve(10);
        }

        m_buffer.push_back(static_cast<char>(0x80 | (static_cast<uint8_t>(opcode) & 0x0F)));
        if (payload_len < 126) {
            m_buffer.push_back(static_cast<char>(payload_len));
            return;
        }

        if (payload_len <= 0xFFFF) {
            m_buffer.push_back(static_cast<char>(126));
            m_buffer.push_back(static_cast<char>((payload_len >> 8) & 0xFF));
            m_buffer.push_back(static_cast<char>(payload_len & 0xFF));
            return;
        }

        m_buffer.push_back(static_cast<char>(127));
        for (int i = 7; i >= 0; --i) {
            m_buffer.push_back(static_cast<char>((payload_len >> (i * 8)) & 0xFF));
        }
    }

    void prepareSendFrame(const WsFrame& frame) {
        if constexpr (is_tcp_socket_v<SocketType>) {
            if (!tryPrepareCommonTcpFrame(frame)) {
                prepareWritevBuffers(frame);
            }
        } else {
            WsFrameParser::encodeInto(m_buffer, frame, m_setting.use_mask);
            m_remaining_bytes = m_buffer.size();
        }
    }

    void prepareSendFrame(WsFrame&& frame) {
        if constexpr (is_tcp_socket_v<SocketType>) {
            if (!tryPrepareCommonTcpFrame(frame)) {
                prepareWritevBuffers(std::move(frame));
            }
        } else {
            WsFrameParser::encodeInto(m_buffer, frame, m_setting.use_mask);
            m_remaining_bytes = m_buffer.size();
        }
    }

    void prepareWritevBuffers(const WsFrame& frame) {
        m_buffer = WsFrameParser::toBytesHeader(frame, m_setting.use_mask, m_masking_key);
        m_payload_buffer = frame.payload;
        finalizeWritevBuffers(false);
    }

    void prepareWritevBuffers(WsFrame&& frame) {
        m_buffer = WsFrameParser::toBytesHeader(frame, m_setting.use_mask, m_masking_key);
        m_payload_buffer = std::move(frame.payload);
        finalizeWritevBuffers(false);
    }

    void finalizeWritevBuffers(bool used_fast_path) {
        if (m_setting.use_mask && !m_payload_buffer.empty()) {
            WsFrameParser::applyMask(m_payload_buffer, m_masking_key);
        }

        m_writev_cursor.clear();
        m_writev_cursor.append({const_cast<char*>(m_buffer.data()), m_buffer.size()});
        if (!m_payload_buffer.empty()) {
            m_writev_cursor.append({const_cast<char*>(m_payload_buffer.data()), m_payload_buffer.size()});
        }

        m_remaining_bytes = m_writev_cursor.remainingBytes();
        if (used_fast_path) {
            ++m_fast_path_counters.hits;
        } else {
            ++m_fast_path_counters.fallbacks;
        }
    }

public:
    void resetPendingState() {
        m_buffer.clear();
        m_payload_buffer.clear();
        m_writev_cursor.clear();
        m_remaining_bytes = 0;
    }

    void updateRemaining(size_t bytes_sent) {
        if (bytes_sent >= m_remaining_bytes) {
            m_remaining_bytes = 0;
            m_buffer.clear();
        } else {
            m_remaining_bytes -= bytes_sent;
        }
    }

    void updateRemainingWritev(size_t bytes_sent) {
        const size_t advanced = m_writev_cursor.advance(bytes_sent);
        if (advanced >= m_remaining_bytes) {
            m_remaining_bytes = 0;
            m_buffer.clear();
            m_payload_buffer.clear();
            m_writev_cursor.clear();
            return;
        }

        m_remaining_bytes -= advanced;
    }

    size_t getRemainingBytes() const {
        return m_remaining_bytes;
    }

    const char* bufferData() const {
        return m_buffer.data();
    }

    size_t sentBytes() const {
        return m_buffer.size() - m_remaining_bytes;
    }

    const iovec* getIovecsData() const {
        return m_writev_cursor.data();
    }

    size_t getIovecsCount() const {
        return m_writev_cursor.count();
    }

private:
    WsWriterSetting m_setting;
    SocketType* m_socket;
    std::string m_buffer;
    std::string m_payload_buffer;
    IoVecCursor m_writev_cursor;
    size_t m_remaining_bytes;
    OperationCounters m_operation_counters;
    FastPathCounters m_fast_path_counters;
    uint8_t m_masking_key[4];

    friend struct detail::WsEchoMachine<SocketType>;
    friend struct detail::WsSslEchoMachine<SocketType>;
};

using WsWriter = WsWriterImpl<TcpSocket>;

} // namespace galay::websocket

#ifdef GALAY_SSL_FEATURE_ENABLED
namespace galay::websocket {
using WssWriter = WsWriterImpl<galay::ssl::SslSocket>;
} // namespace galay::websocket
#endif

#endif // GALAY_WS_WRITER_H
