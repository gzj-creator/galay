/**
 * @file ws_conn.h
 * @brief WebSocket 连接，封装底层资源和回显功能
 * @author galay-http
 * @version 1.0.0
 *
 * @details 提供 WsConnImpl 模板类，封装 WebSocket 连接的底层 Socket 和 RingBuffer，
 *          支持读取器/写入器获取和零拷贝回显操作。
 */

#ifndef GALAY_WS_CONN_H
#define GALAY_WS_CONN_H

#include "ws_reader.h"
#include "ws_writer.h"
#include "galay-http/kernel/http_conn.h"
#include "galay-kernel/async/tcp_socket.h"
#include <galay-utils/cache/bytes.hpp>
#include <galay-utils/cache/ring_buffer.hpp>

namespace galay::websocket
{

using namespace galay::async;
using namespace galay::kernel;
using ::galay::utils::Bytes;
using ::galay::utils::RingBuffer;

template<typename SocketType>
class WsConnImpl;

namespace detail {

inline size_t encodeServerEchoHeader(char out[10], WsOpcode opcode, size_t payload_size) noexcept {
    size_t header_size = 0;
    out[header_size++] = static_cast<char>(0x80 | (static_cast<uint8_t>(opcode) & 0x0F));
    if (payload_size < 126) {
        out[header_size++] = static_cast<char>(payload_size);
        return header_size;
    }

    if (payload_size <= 0xFFFF) {
        out[header_size++] = 126;
        out[header_size++] = static_cast<char>((payload_size >> 8) & 0xFF);
        out[header_size++] = static_cast<char>(payload_size & 0xFF);
        return header_size;
    }

    out[header_size++] = 127;
    for (int i = 7; i >= 0; --i) {
        out[header_size++] = static_cast<char>((static_cast<uint64_t>(payload_size) >> (i * 8)) & 0xFF);
    }
    return header_size;
}

template<typename SocketType>
struct WsEchoMachine {
    using result_type = std::expected<bool, WsError>;
    static constexpr auto kSequenceOwnerDomain = galay::kernel::SequenceOwnerDomain::ReadWrite;

    struct DirectSendState {
        char header[10] = {0};
        size_t header_size = 0;
        size_t consume_bytes = 0;
        char* data = nullptr;
        size_t total_bytes = 0;
        size_t sent_bytes = 0;
        IoVecCursor cursor;

        bool useContiguous() const noexcept { return data != nullptr && total_bytes > 0; }
        bool useCursor() const noexcept { return !cursor.empty(); }
        bool active() const noexcept { return useContiguous() || useCursor(); }

        void reset() noexcept {
            header_size = 0;
            consume_bytes = 0;
            data = nullptr;
            total_bytes = 0;
            sent_bytes = 0;
            cursor.clear();
        }
    };

    static WsWriterSetting resolveWriterSetting(WsConnImpl<SocketType>* conn, WsWriterSetting setting) {
        setting.use_mask = !conn->m_is_server;
        return setting;
    }

    WsEchoMachine(WsConnImpl<SocketType>* conn,
                  const WsReaderSetting& reader_setting,
                  WsWriterSetting writer_setting,
                  std::string& message,
                  WsOpcode& opcode,
                  bool preserve_message = true)
        : m_conn(conn)
        , m_reader_setting(reader_setting)
        , m_read_state(conn->m_ring_buffer,
                       m_reader_setting,
                       message,
                       opcode,
                       conn->m_is_server,
                       !conn->m_is_server,
                       nullptr)
        , m_writer(resolveWriterSetting(conn, writer_setting), conn->m_socket)
        , m_message(&message)
        , m_opcode(&opcode)
        , m_preserve_message(preserve_message) {}

    MachineAction<result_type> advance() {
        ++m_conn->m_echo_counters.composite_advance_calls;
        if (m_result.has_value()) {
            return MachineAction<result_type>::complete(std::move(*m_result));
        }

        if (m_stage == Stage::kRead) {
            return advanceRead();
        }
        return advanceWrite();
    }

    void onRead(std::expected<size_t, IOError> result) {
        if (!result) {
            m_read_state.setRecvError(result.error());
            m_result = m_read_state.takeResult();
            return;
        }

        if (result.value() == 0) {
            m_read_state.onPeerClosed();
            m_result = m_read_state.takeResult();
            return;
        }

        m_read_state.onBytesReceived(result.value());
    }

    void onWrite(std::expected<size_t, IOError> result) {
        if (m_direct_send.active()) {
            if (!result) {
                m_result = std::unexpected(WsError(kWsSendError, result.error().message()));
                return;
            }

            if (m_direct_send.useContiguous()) {
                m_direct_send.sent_bytes += result.value();
            } else {
                m_direct_send.cursor.advance(result.value());
            }

            if ((m_direct_send.useContiguous() && m_direct_send.sent_bytes >= m_direct_send.total_bytes) ||
                (m_direct_send.useCursor() && m_direct_send.cursor.empty())) {
                m_conn->m_ring_buffer.consume(m_direct_send.consume_bytes);
                m_direct_send.reset();
                m_result = true;
            }
            return;
        }

        if (!result) {
            m_result = std::unexpected(WsError(kWsSendError, result.error().message()));
            return;
        }

        const size_t written = result.value();
        if (written > 0) {
            m_writer.updateRemainingWritev(written);
        }

        if (m_writer.getRemainingBytes() == 0) {
            m_result = true;
            return;
        }

        if (m_writer.getIovecsData() == nullptr || m_writer.getIovecsCount() == 0) {
            m_result = std::unexpected(WsError(kWsSendError, "No remaining iovec to write"));
        }
    }

private:
    enum class Stage : uint8_t {
        kRead,
        kWrite,
    };

    MachineAction<result_type> advanceRead() {
        if (!m_preserve_message && tryPrepareZeroCopy()) {
            m_stage = Stage::kWrite;
            return advanceWrite();
        }

        if (m_read_state.parseFromBuffer()) {
            return onParsedMessage();
        }

        if (!m_read_state.prepareRecvWindow()) {
            m_result = m_read_state.takeResult();
            return MachineAction<result_type>::complete(std::move(*m_result));
        }

        return MachineAction<result_type>::waitReadv(
            m_read_state.recvIovecsData(),
            m_read_state.recvIovecsCount());
    }

    MachineAction<result_type> onParsedMessage() {
        auto parsed = m_read_state.takeResult();
        if (!parsed.has_value()) {
            m_result = std::unexpected(parsed.error());
            return MachineAction<result_type>::complete(std::move(*m_result));
        }

        if (*m_opcode == WsOpcode::Text) {
            ++m_conn->m_echo_counters.composite_hits;
            if (m_preserve_message) {
                m_writer.prepareSendFrame(WsFrameParser::createTextFrame(*m_message));
            } else {
                m_writer.prepareSendFrame(WsFrameParser::createTextFrame(std::move(*m_message)));
            }
            m_stage = Stage::kWrite;
            return advanceWrite();
        }

        if (*m_opcode == WsOpcode::Binary) {
            ++m_conn->m_echo_counters.composite_hits;
            if (m_preserve_message) {
                m_writer.prepareSendFrame(WsFrameParser::createBinaryFrame(*m_message));
            } else {
                m_writer.prepareSendFrame(WsFrameParser::createBinaryFrame(std::move(*m_message)));
            }
            m_stage = Stage::kWrite;
            return advanceWrite();
        }

        ++m_conn->m_echo_counters.composite_fallbacks;
        m_result = true;
        return MachineAction<result_type>::complete(true);
    }

    MachineAction<result_type> advanceWrite() {
        if (m_direct_send.active()) {
            if (m_direct_send.useContiguous()) {
                if (m_direct_send.sent_bytes >= m_direct_send.total_bytes) {
                    m_conn->m_ring_buffer.consume(m_direct_send.consume_bytes);
                    m_direct_send.reset();
                    m_result = true;
                    return MachineAction<result_type>::complete(true);
                }

                m_direct_iovec = {
                    m_direct_send.data + m_direct_send.sent_bytes,
                    m_direct_send.total_bytes - m_direct_send.sent_bytes,
                };
                return MachineAction<result_type>::waitWritev(&m_direct_iovec, 1);
            }

            if (m_direct_send.cursor.empty()) {
                m_conn->m_ring_buffer.consume(m_direct_send.consume_bytes);
                m_direct_send.reset();
                m_result = true;
                return MachineAction<result_type>::complete(true);
            }

            return MachineAction<result_type>::waitWritev(
                m_direct_send.cursor.data(),
                m_direct_send.cursor.count());
        }

        if (m_writer.getRemainingBytes() == 0) {
            m_result = true;
            return MachineAction<result_type>::complete(true);
        }

        const auto* iov_data = m_writer.getIovecsData();
        const auto iov_count = m_writer.getIovecsCount();
        if (iov_data == nullptr || iov_count == 0) {
            m_result = std::unexpected(WsError(kWsSendError, "No remaining iovec to write"));
            return MachineAction<result_type>::complete(std::move(*m_result));
        }
        return MachineAction<result_type>::waitWritev(iov_data, iov_count);
    }

    bool tryPrepareZeroCopy() {
        auto read_iovecs = borrowReadIovecs(m_conn->m_ring_buffer);
        WsConsumeFastPathView view;
        if (!bindWsConsumeFastPathView(read_iovecs.data(), read_iovecs.size(), m_conn->m_is_server, view)) {
            return false;
        }

        if (view.opcode == WsOpcode::Text &&
            ((view.payload_iovecs.size() == 1)
                 ? !wsIsValidUtf8MaskedSpan(view.payload_data, view.payload_length, view.masking_key)
                 : !wsIsValidUtf8MaskedIovecs(view.payload_iovecs.data(),
                                             view.payload_iovecs.size(),
                                             view.masking_key))) {
            return false;
        }

        const size_t header_size = encodeServerEchoHeader(
            m_direct_send.header,
            view.opcode,
            view.payload_length);

        if (view.payload_iovecs.size() == 1 &&
            view.payload_data != nullptr &&
            view.payload_offset >= header_size) {
            wsApplyMaskInPlace(view.payload_data, view.payload_length, view.masking_key);
            std::memcpy(view.payload_data - header_size, m_direct_send.header, header_size);

            *m_opcode = view.opcode;
            m_message->clear();
            ++m_conn->m_echo_counters.composite_hits;
            ++m_conn->m_echo_counters.zero_copy_hits;
            m_direct_send.reset();
            m_direct_send.data = view.payload_data - header_size;
            m_direct_send.total_bytes = header_size + view.payload_length;
            m_direct_send.sent_bytes = 0;
            m_direct_send.consume_bytes = view.frame_length;
            return true;
        }

        wsApplyMaskIovecsInPlace(view.payload_iovecs.data(), view.payload_iovecs.size(), view.masking_key);

        *m_opcode = view.opcode;
        m_message->clear();
        ++m_conn->m_echo_counters.composite_hits;
        ++m_conn->m_echo_counters.zero_copy_hits;
        m_direct_send.reset();
        m_direct_send.header_size = header_size;
        m_direct_send.consume_bytes = view.frame_length;
        m_direct_send.cursor.reserve(3);
        m_direct_send.cursor.append({
            m_direct_send.header,
            m_direct_send.header_size,
        });
        for (size_t i = 0; i < view.payload_iovecs.size(); ++i) {
            m_direct_send.cursor.append(view.payload_iovecs[i]);
        }
        return true;
    }

    WsConnImpl<SocketType>* m_conn;
    WsReaderSetting m_reader_setting;
    WsMessageReadState m_read_state;
    WsWriterImpl<SocketType> m_writer;
    std::string* m_message;
    WsOpcode* m_opcode;
    bool m_preserve_message = true;
    DirectSendState m_direct_send;
    struct iovec m_direct_iovec{};
    Stage m_stage = Stage::kRead;
    std::optional<result_type> m_result;
};

#ifdef GALAY_SSL_FEATURE_ENABLED
template<typename SocketType>
struct WsSslEchoMachine {
    using result_type = std::expected<bool, WsError>;
    static constexpr auto kSequenceOwnerDomain = galay::kernel::SequenceOwnerDomain::ReadWrite;

    using DirectSendState = typename WsEchoMachine<SocketType>::DirectSendState;

    static WsWriterSetting resolveWriterSetting(WsConnImpl<SocketType>* conn, WsWriterSetting setting) {
        setting.use_mask = !conn->m_is_server;
        return setting;
    }

    WsSslEchoMachine(WsConnImpl<SocketType>* conn,
                     const WsReaderSetting& reader_setting,
                     WsWriterSetting writer_setting,
                     std::string& message,
                     WsOpcode& opcode,
                     bool preserve_message = true)
        : m_conn(conn)
        , m_reader_setting(reader_setting)
        , m_read_state(conn->m_ring_buffer,
                       m_reader_setting,
                       message,
                       opcode,
                       conn->m_is_server,
                       !conn->m_is_server,
                       nullptr)
        , m_writer(resolveWriterSetting(conn, writer_setting), conn->m_socket)
        , m_message(&message)
        , m_opcode(&opcode)
        , m_preserve_message(preserve_message) {}

    galay::ssl::SslMachineAction<result_type> advance() {
        ++m_conn->m_echo_counters.composite_advance_calls;
        if (m_result.has_value()) {
            return galay::ssl::SslMachineAction<result_type>::complete(std::move(*m_result));
        }

        if (m_stage == Stage::kRead) {
            return advanceRead();
        }
        return advanceWrite();
    }

    void onHandshake(std::expected<void, galay::ssl::SslError>) {}

    void onRecv(std::expected<Bytes, galay::ssl::SslError> result) {
        if (!result) {
            m_read_state.setSslRecvError(result.error());
            m_result = m_read_state.takeResult();
            return;
        }

        const size_t recv_bytes = result.value().size();
        if (recv_bytes == 0) {
            m_read_state.onPeerClosed();
            m_result = m_read_state.takeResult();
            return;
        }

        m_read_state.onBytesReceived(recv_bytes);
    }

    void onSend(std::expected<size_t, galay::ssl::SslError> result) {
        if (m_direct_send.active()) {
            if (!result) {
                m_result = std::unexpected(WsError(kWsSendError, result.error().message()));
                return;
            }

            if (result.value() == 0) {
                m_result = std::unexpected(WsError(kWsSendError, "SSL send returned zero bytes"));
                return;
            }

            if (m_direct_send.useContiguous()) {
                m_direct_send.sent_bytes += result.value();
            } else {
                m_direct_send.cursor.advance(result.value());
            }

            if ((m_direct_send.useContiguous() && m_direct_send.sent_bytes >= m_direct_send.total_bytes) ||
                (m_direct_send.useCursor() && m_direct_send.cursor.empty())) {
                m_conn->m_ring_buffer.consume(m_direct_send.consume_bytes);
                m_direct_send.reset();
                m_result = true;
            }
            return;
        }

        if (!result) {
            m_writer.resetPendingState();
            m_result = std::unexpected(WsError(kWsSendError, result.error().message()));
            return;
        }

        if (result.value() == 0) {
            m_writer.resetPendingState();
            m_result = std::unexpected(WsError(kWsSendError, "SSL send returned zero bytes"));
            return;
        }

        m_writer.updateRemaining(result.value());
        if (m_writer.getRemainingBytes() == 0) {
            m_result = true;
        }
    }

    void onShutdown(std::expected<void, galay::ssl::SslError>) {}

private:
    enum class Stage : uint8_t {
        kRead,
        kWrite,
    };

    galay::ssl::SslMachineAction<result_type> advanceRead() {
        if (!m_preserve_message && tryPrepareZeroCopy()) {
            m_stage = Stage::kWrite;
            return advanceWrite();
        }

        if (m_read_state.parseFromBuffer()) {
            return onParsedMessage();
        }

        char* recv_buffer = nullptr;
        size_t recv_length = 0;
        if (!m_read_state.prepareRecvWindow(recv_buffer, recv_length)) {
            m_result = m_read_state.takeResult();
            return galay::ssl::SslMachineAction<result_type>::complete(std::move(*m_result));
        }

        return galay::ssl::SslMachineAction<result_type>::recv(recv_buffer, recv_length);
    }

    galay::ssl::SslMachineAction<result_type> onParsedMessage() {
        auto parsed = m_read_state.takeResult();
        if (!parsed.has_value()) {
            m_result = std::unexpected(parsed.error());
            return galay::ssl::SslMachineAction<result_type>::complete(std::move(*m_result));
        }

        if (*m_opcode == WsOpcode::Text) {
            ++m_conn->m_echo_counters.composite_hits;
            ++m_conn->m_echo_counters.ssl_direct_message_hits;
            if (m_preserve_message) {
                m_writer.prepareSslMessage(WsOpcode::Text, *m_message);
            } else {
                m_writer.prepareSslMessage(WsOpcode::Text, std::move(*m_message));
            }
            m_stage = Stage::kWrite;
            return advanceWrite();
        }

        if (*m_opcode == WsOpcode::Binary) {
            ++m_conn->m_echo_counters.composite_hits;
            ++m_conn->m_echo_counters.ssl_direct_message_hits;
            if (m_preserve_message) {
                m_writer.prepareSslMessage(WsOpcode::Binary, *m_message);
            } else {
                m_writer.prepareSslMessage(WsOpcode::Binary, std::move(*m_message));
            }
            m_stage = Stage::kWrite;
            return advanceWrite();
        }

        ++m_conn->m_echo_counters.composite_fallbacks;
        m_result = true;
        return galay::ssl::SslMachineAction<result_type>::complete(true);
    }

    galay::ssl::SslMachineAction<result_type> advanceWrite() {
        if (m_direct_send.active()) {
            if (m_direct_send.useContiguous()) {
                if (m_direct_send.sent_bytes >= m_direct_send.total_bytes) {
                    m_conn->m_ring_buffer.consume(m_direct_send.consume_bytes);
                    m_direct_send.reset();
                    m_result = true;
                    return galay::ssl::SslMachineAction<result_type>::complete(true);
                }

                return galay::ssl::SslMachineAction<result_type>::send(
                    m_direct_send.data + m_direct_send.sent_bytes,
                    m_direct_send.total_bytes - m_direct_send.sent_bytes);
            }

            if (m_direct_send.cursor.empty()) {
                m_conn->m_ring_buffer.consume(m_direct_send.consume_bytes);
                m_direct_send.reset();
                m_result = true;
                return galay::ssl::SslMachineAction<result_type>::complete(true);
            }

            const auto* direct_iov = m_direct_send.cursor.data();
            return galay::ssl::SslMachineAction<result_type>::send(
                static_cast<const char*>(direct_iov->iov_base),
                direct_iov->iov_len);
        }

        if (m_writer.getRemainingBytes() == 0) {
            m_result = true;
            return galay::ssl::SslMachineAction<result_type>::complete(true);
        }

        return galay::ssl::SslMachineAction<result_type>::send(
            m_writer.bufferData() + m_writer.sentBytes(),
            m_writer.getRemainingBytes());
    }

    bool tryPrepareZeroCopy() {
        auto read_iovecs = borrowReadIovecs(m_conn->m_ring_buffer);
        WsConsumeFastPathView view;
        if (!bindWsConsumeFastPathView(read_iovecs.data(), read_iovecs.size(), m_conn->m_is_server, view)) {
            return false;
        }

        if (view.opcode == WsOpcode::Text &&
            ((view.payload_iovecs.size() == 1)
                 ? !wsIsValidUtf8MaskedSpan(view.payload_data, view.payload_length, view.masking_key)
                 : !wsIsValidUtf8MaskedIovecs(view.payload_iovecs.data(),
                                             view.payload_iovecs.size(),
                                             view.masking_key))) {
            return false;
        }

        const size_t header_size = encodeServerEchoHeader(
            m_direct_send.header,
            view.opcode,
            view.payload_length);

        if (view.payload_iovecs.size() == 1 &&
            view.payload_data != nullptr &&
            view.payload_offset >= header_size) {
            wsApplyMaskInPlace(view.payload_data, view.payload_length, view.masking_key);
            std::memcpy(view.payload_data - header_size, m_direct_send.header, header_size);

            *m_opcode = view.opcode;
            m_message->clear();
            ++m_conn->m_echo_counters.composite_hits;
            ++m_conn->m_echo_counters.zero_copy_hits;
            m_direct_send.reset();
            m_direct_send.data = view.payload_data - header_size;
            m_direct_send.total_bytes = header_size + view.payload_length;
            m_direct_send.sent_bytes = 0;
            m_direct_send.consume_bytes = view.frame_length;
            return true;
        }

        wsApplyMaskIovecsInPlace(view.payload_iovecs.data(), view.payload_iovecs.size(), view.masking_key);

        *m_opcode = view.opcode;
        m_message->clear();
        ++m_conn->m_echo_counters.composite_hits;
        ++m_conn->m_echo_counters.zero_copy_hits;
        m_direct_send.reset();
        m_direct_send.header_size = header_size;
        m_direct_send.consume_bytes = view.frame_length;
        m_direct_send.cursor.reserve(3);
        m_direct_send.cursor.append({
            m_direct_send.header,
            m_direct_send.header_size,
        });
        for (size_t i = 0; i < view.payload_iovecs.size(); ++i) {
            m_direct_send.cursor.append(view.payload_iovecs[i]);
        }
        return true;
    }

    WsConnImpl<SocketType>* m_conn;
    WsReaderSetting m_reader_setting;
    WsMessageReadState m_read_state;
    WsWriterImpl<SocketType> m_writer;
    std::string* m_message;
    WsOpcode* m_opcode;
    bool m_preserve_message = true;
    DirectSendState m_direct_send;
    Stage m_stage = Stage::kRead;
    std::optional<result_type> m_result;
};

template<typename SocketType>
struct WsSslEchoLoopMachine {
    using result_type = std::expected<bool, WsError>;
    static constexpr auto kSequenceOwnerDomain = galay::kernel::SequenceOwnerDomain::ReadWrite;

    using DirectSendState = typename WsEchoMachine<SocketType>::DirectSendState;

    static WsWriterSetting resolveWriterSetting(WsConnImpl<SocketType>* conn, WsWriterSetting setting) {
        setting.use_mask = !conn->m_is_server;
        return setting;
    }

    explicit WsSslEchoLoopMachine(WsConnImpl<SocketType>* conn,
                                  const WsReaderSetting& reader_setting,
                                  WsWriterSetting writer_setting)
        : m_conn(conn)
        , m_reader_setting(reader_setting)
        , m_message()
        , m_opcode(WsOpcode::Close)
        , m_read_state(conn->m_ring_buffer,
                       m_reader_setting,
                       m_message,
                       m_opcode,
                       conn->m_is_server,
                       !conn->m_is_server,
                       nullptr)
        , m_writer(resolveWriterSetting(conn, writer_setting), conn->m_socket) {}

    WsSslEchoLoopMachine(const WsSslEchoLoopMachine&) = delete;
    WsSslEchoLoopMachine& operator=(const WsSslEchoLoopMachine&) = delete;

    WsSslEchoLoopMachine(WsSslEchoLoopMachine&& other)
        : m_conn(other.m_conn)
        , m_reader_setting(std::move(other.m_reader_setting))
        , m_message(std::move(other.m_message))
        , m_opcode(other.m_opcode)
        , m_read_state(std::move(other.m_read_state))
        , m_writer(std::move(other.m_writer))
        , m_direct_send(std::move(other.m_direct_send))
        , m_control_buffer(std::move(other.m_control_buffer))
        , m_control_sent_bytes(other.m_control_sent_bytes)
        , m_close_after_send(other.m_close_after_send)
        , m_stage(other.m_stage)
        , m_write_mode(other.m_write_mode)
        , m_result(std::move(other.m_result))
    {
        rebindOwnedReadState();
    }

    WsSslEchoLoopMachine& operator=(WsSslEchoLoopMachine&& other) {
        if (this == &other) {
            return *this;
        }

        m_conn = other.m_conn;
        m_reader_setting = std::move(other.m_reader_setting);
        m_message = std::move(other.m_message);
        m_opcode = other.m_opcode;
        m_read_state = std::move(other.m_read_state);
        m_writer = std::move(other.m_writer);
        m_direct_send = std::move(other.m_direct_send);
        m_control_buffer = std::move(other.m_control_buffer);
        m_control_sent_bytes = other.m_control_sent_bytes;
        m_close_after_send = other.m_close_after_send;
        m_stage = other.m_stage;
        m_write_mode = other.m_write_mode;
        m_result = std::move(other.m_result);
        rebindOwnedReadState();
        return *this;
    }

    galay::ssl::SslMachineAction<result_type> advance() {
        if (m_result.has_value()) {
            return galay::ssl::SslMachineAction<result_type>::complete(std::move(*m_result));
        }

        if (m_stage == Stage::kRead) {
            return advanceRead();
        }
        return advanceWrite();
    }

    void onHandshake(std::expected<void, galay::ssl::SslError>) {}

    void onRecv(std::expected<Bytes, galay::ssl::SslError> result) {
        if (!result) {
            m_read_state.setSslRecvError(result.error());
            m_result = m_read_state.takeResult();
            return;
        }

        const size_t recv_bytes = result.value().size();
        if (recv_bytes == 0) {
            m_read_state.onPeerClosed();
            m_result = m_read_state.takeResult();
            return;
        }

        m_read_state.onBytesReceived(recv_bytes);
    }

    void onSend(std::expected<size_t, galay::ssl::SslError> result) {
        if (!result) {
            if (m_write_mode == WriteMode::kWriter) {
                m_writer.resetPendingState();
            }
            m_result = std::unexpected(WsError(kWsSendError, result.error().message()));
            return;
        }

        if (result.value() == 0) {
            if (m_write_mode == WriteMode::kWriter) {
                m_writer.resetPendingState();
            }
            m_result = std::unexpected(WsError(kWsSendError, "SSL send returned zero bytes"));
            return;
        }

        switch (m_write_mode) {
            case WriteMode::kDirect:
                if (m_direct_send.useContiguous()) {
                    m_direct_send.sent_bytes += result.value();
                } else {
                    m_direct_send.cursor.advance(result.value());
                }

                if ((m_direct_send.useContiguous() && m_direct_send.sent_bytes >= m_direct_send.total_bytes) ||
                    (m_direct_send.useCursor() && m_direct_send.cursor.empty())) {
                    m_conn->m_ring_buffer.consume(m_direct_send.consume_bytes);
                    m_direct_send.reset();
                    finishCurrentMessage();
                }
                return;
            case WriteMode::kWriter:
                m_writer.updateRemaining(result.value());
                if (m_writer.getRemainingBytes() == 0) {
                    finishCurrentMessage();
                }
                return;
            case WriteMode::kControl:
                m_control_sent_bytes += result.value();
                if (m_control_sent_bytes >= m_control_buffer.size()) {
                    if (m_close_after_send) {
                        m_result = true;
                    } else {
                        finishCurrentMessage();
                    }
                }
                return;
            case WriteMode::kNone:
                m_result = std::unexpected(WsError(kWsSendError, "Unexpected SSL send completion"));
                return;
        }
    }

    void onShutdown(std::expected<void, galay::ssl::SslError>) {}

private:
    enum class Stage : uint8_t {
        kRead,
        kWrite,
    };

    enum class WriteMode : uint8_t {
        kNone,
        kDirect,
        kWriter,
        kControl,
    };

    galay::ssl::SslMachineAction<result_type> advanceRead() {
        if (tryPrepareZeroCopy()) {
            m_stage = Stage::kWrite;
            m_write_mode = WriteMode::kDirect;
            return advanceWrite();
        }

        if (m_read_state.parseFromBuffer()) {
            return onParsedMessage();
        }

        char* recv_buffer = nullptr;
        size_t recv_length = 0;
        if (!m_read_state.prepareRecvWindow(recv_buffer, recv_length)) {
            m_result = m_read_state.takeResult();
            return galay::ssl::SslMachineAction<result_type>::complete(std::move(*m_result));
        }

        return galay::ssl::SslMachineAction<result_type>::recv(recv_buffer, recv_length);
    }

    galay::ssl::SslMachineAction<result_type> onParsedMessage() {
        auto parsed = m_read_state.takeResult();
        if (!parsed.has_value()) {
            m_result = std::unexpected(parsed.error());
            return galay::ssl::SslMachineAction<result_type>::complete(std::move(*m_result));
        }

        switch (m_opcode) {
            case WsOpcode::Text:
                ++m_conn->m_echo_counters.composite_hits;
                ++m_conn->m_echo_counters.ssl_direct_message_hits;
                m_writer.prepareSslMessage(WsOpcode::Text, std::move(m_message));
                m_write_mode = WriteMode::kWriter;
                m_stage = Stage::kWrite;
                return advanceWrite();
            case WsOpcode::Binary:
                ++m_conn->m_echo_counters.composite_hits;
                ++m_conn->m_echo_counters.ssl_direct_message_hits;
                m_writer.prepareSslMessage(WsOpcode::Binary, std::move(m_message));
                m_write_mode = WriteMode::kWriter;
                m_stage = Stage::kWrite;
                return advanceWrite();
            case WsOpcode::Ping:
                m_writer.prepareSslMessage(WsOpcode::Pong, m_message);
                m_write_mode = WriteMode::kWriter;
                m_stage = Stage::kWrite;
                return advanceWrite();
            case WsOpcode::Close:
                m_control_buffer = WsFrameParser::toBytes(
                    WsFrameParser::createCloseFrame(WsCloseCode::Normal),
                    false);
                m_control_sent_bytes = 0;
                m_close_after_send = true;
                m_write_mode = WriteMode::kControl;
                m_stage = Stage::kWrite;
                return advanceWrite();
            default:
                finishCurrentMessage();
                return advanceRead();
        }
    }

    galay::ssl::SslMachineAction<result_type> advanceWrite() {
        switch (m_write_mode) {
            case WriteMode::kDirect:
                if (m_direct_send.useContiguous()) {
                    if (m_direct_send.sent_bytes >= m_direct_send.total_bytes) {
                        m_conn->m_ring_buffer.consume(m_direct_send.consume_bytes);
                        m_direct_send.reset();
                        finishCurrentMessage();
                        return advanceRead();
                    }

                    return galay::ssl::SslMachineAction<result_type>::send(
                        m_direct_send.data + m_direct_send.sent_bytes,
                        m_direct_send.total_bytes - m_direct_send.sent_bytes);
                }

                if (m_direct_send.cursor.empty()) {
                    m_conn->m_ring_buffer.consume(m_direct_send.consume_bytes);
                    m_direct_send.reset();
                    finishCurrentMessage();
                    return advanceRead();
                }

                return galay::ssl::SslMachineAction<result_type>::send(
                    static_cast<const char*>(m_direct_send.cursor.data()->iov_base),
                    m_direct_send.cursor.data()->iov_len);
            case WriteMode::kWriter:
                if (m_writer.getRemainingBytes() == 0) {
                    finishCurrentMessage();
                    return advanceRead();
                }

                return galay::ssl::SslMachineAction<result_type>::send(
                    m_writer.bufferData() + m_writer.sentBytes(),
                    m_writer.getRemainingBytes());
            case WriteMode::kControl:
                if (m_control_sent_bytes >= m_control_buffer.size()) {
                    if (m_close_after_send) {
                        m_result = true;
                        return galay::ssl::SslMachineAction<result_type>::complete(true);
                    }
                    finishCurrentMessage();
                    return advanceRead();
                }

                return galay::ssl::SslMachineAction<result_type>::send(
                    m_control_buffer.data() + m_control_sent_bytes,
                    m_control_buffer.size() - m_control_sent_bytes);
            case WriteMode::kNone:
                m_result = true;
                return galay::ssl::SslMachineAction<result_type>::complete(true);
        }

        m_result = std::unexpected(WsError(kWsSendError, "Invalid SSL echo loop write mode"));
        return galay::ssl::SslMachineAction<result_type>::complete(std::move(*m_result));
    }

    bool tryPrepareZeroCopy() {
        auto read_iovecs = borrowReadIovecs(m_conn->m_ring_buffer);
        WsConsumeFastPathView view;
        if (!bindWsConsumeFastPathView(read_iovecs.data(), read_iovecs.size(), m_conn->m_is_server, view)) {
            return false;
        }

        if (view.opcode == WsOpcode::Text &&
            ((view.payload_iovecs.size() == 1)
                 ? !wsIsValidUtf8MaskedSpan(view.payload_data, view.payload_length, view.masking_key)
                 : !wsIsValidUtf8MaskedIovecs(view.payload_iovecs.data(),
                                             view.payload_iovecs.size(),
                                             view.masking_key))) {
            return false;
        }

        const size_t header_size = encodeServerEchoHeader(
            m_direct_send.header,
            view.opcode,
            view.payload_length);

        if (view.payload_iovecs.size() == 1 &&
            view.payload_data != nullptr &&
            view.payload_offset >= header_size) {
            wsApplyMaskInPlace(view.payload_data, view.payload_length, view.masking_key);
            std::memcpy(view.payload_data - header_size, m_direct_send.header, header_size);

            m_opcode = view.opcode;
            m_message.clear();
            ++m_conn->m_echo_counters.composite_hits;
            ++m_conn->m_echo_counters.zero_copy_hits;
            m_direct_send.reset();
            m_direct_send.data = view.payload_data - header_size;
            m_direct_send.total_bytes = header_size + view.payload_length;
            m_direct_send.sent_bytes = 0;
            m_direct_send.consume_bytes = view.frame_length;
            return true;
        }

        wsApplyMaskIovecsInPlace(view.payload_iovecs.data(), view.payload_iovecs.size(), view.masking_key);

        m_opcode = view.opcode;
        m_message.clear();
        ++m_conn->m_echo_counters.composite_hits;
        ++m_conn->m_echo_counters.zero_copy_hits;
        m_direct_send.reset();
        m_direct_send.header_size = header_size;
        m_direct_send.consume_bytes = view.frame_length;
        m_direct_send.cursor.reserve(3);
        m_direct_send.cursor.append({
            m_direct_send.header,
            m_direct_send.header_size,
        });
        for (size_t i = 0; i < view.payload_iovecs.size(); ++i) {
            m_direct_send.cursor.append(view.payload_iovecs[i]);
        }
        return true;
    }

    void finishCurrentMessage() {
        m_close_after_send = false;
        m_control_buffer.clear();
        m_control_sent_bytes = 0;
        m_write_mode = WriteMode::kNone;
        m_stage = Stage::kRead;
        m_read_state.resetForNextMessage();
    }

    void rebindOwnedReadState() noexcept {
        m_read_state.rebindStorage(m_message, m_opcode);
    }

    WsConnImpl<SocketType>* m_conn;
    WsReaderSetting m_reader_setting;
    std::string m_message;
    WsOpcode m_opcode = WsOpcode::Close;
    WsMessageReadState m_read_state;
    WsWriterImpl<SocketType> m_writer;
    DirectSendState m_direct_send;
    std::string m_control_buffer;
    size_t m_control_sent_bytes = 0;
    bool m_close_after_send = false;
    Stage m_stage = Stage::kRead;
    WriteMode m_write_mode = WriteMode::kNone;
    std::optional<result_type> m_result;
};
#endif

} // namespace detail

/**
 * @brief WebSocket连接模板类
 * @tparam SocketType Socket类型（TcpSocket 或 SslSocket）
 * @details 封装WebSocket连接的底层资源，不持有reader/writer，通过接口构造返回
 */
template<typename SocketType>
class WsConnImpl
{
public:
    /**
     * @brief 回显操作统计计数器
     */
    struct EchoCounters {
        size_t composite_awaitables_started = 0;  ///< 回显协程启动次数
        size_t composite_hits = 0;                 ///< Text/Binary 帧命中次数
        size_t composite_fallbacks = 0;            ///< 非 Text/Binary 帧回退次数
        size_t composite_advance_calls = 0;        ///< 状态机推进调用次数
        size_t ssl_direct_message_hits = 0;        ///< SSL 直接消息命中次数
        size_t zero_copy_hits = 0;                 ///< 零拷贝命中次数
    };

    /**
     * @brief 从HttpConn构造（用于升级场景）
     * @note 升级之后HttpConn不再可用
     */
    static WsConnImpl<SocketType> from(galay::http::HttpConnImpl<SocketType>&& http_conn, bool is_server = true)
    {
        return WsConnImpl<SocketType>(std::move(http_conn.m_socket), std::move(http_conn.m_ring_buffer), is_server);
    }

    /**
     * @brief 直接构造
     */
    WsConnImpl(SocketType&& socket, RingBuffer&& ring_buffer, bool is_server = true)
        : m_socket(std::move(socket))
        , m_ring_buffer(std::move(ring_buffer))
        , m_is_server(is_server)
    {
    }

    /**
     * @brief 构造函数（只持有socket）
     */
    WsConnImpl(SocketType&& socket, bool is_server = true)
        : m_socket(std::move(socket))
        , m_ring_buffer(8192)  // 默认8KB buffer
        , m_is_server(is_server)
    {
    }

    ~WsConnImpl() = default;

    // 禁用拷贝
    WsConnImpl(const WsConnImpl&) = delete;
    WsConnImpl& operator=(const WsConnImpl&) = delete;

    // 启用移动
    WsConnImpl(WsConnImpl&&) = default;
    WsConnImpl& operator=(WsConnImpl&&) = default;

    /**
     * @brief 关闭连接
     */
    auto close() {
        return m_socket.close();
    }

    /**
     * @brief 获取底层Socket引用
     */
    SocketType& socket() { return m_socket; }

    /**
     * @brief 获取RingBuffer引用
     */
    RingBuffer& ringBuffer() { return m_ring_buffer; }

    /**
     * @brief 获取WsReader
     * @param setting WsReaderSetting配置
     * @return WsReaderImpl<SocketType> Reader对象
     */
    WsReaderImpl<SocketType> getReader(const WsReaderSetting& setting = WsReaderSetting()) {
        // use_mask: 客户端需要mask，服务器不需要
        bool use_mask = !m_is_server;
        return WsReaderImpl<SocketType>(m_ring_buffer, setting, m_socket, m_is_server, use_mask);
    }

    /**
     * @brief 获取WsWriter
     * @param setting WsWriterSetting配置
     * @return WsWriterImpl<SocketType> Writer对象
     */
    WsWriterImpl<SocketType> getWriter(WsWriterSetting setting) {
        // 客户端需要mask，服务器不需要
        setting.use_mask = !m_is_server;
        return WsWriterImpl<SocketType>(setting, m_socket);
    }

    /**
     * @brief 执行一次回显操作（读取并原样发送回消息）
     * @param message 消息内容引用
     * @param opcode 消息操作码引用
     * @param reader_setting 读取器配置
     * @param writer_setting 写入器配置
     * @return 可 co_await 的异步操作，成功返回 true，失败返回 WsError
     */
    auto echoOnce(std::string& message,
                  WsOpcode& opcode,
                  const WsReaderSetting& reader_setting = WsReaderSetting(),
                  WsWriterSetting writer_setting = WsWriterSetting::byServer()) {
        ++m_echo_counters.composite_awaitables_started;
        using ResultType = std::expected<bool, WsError>;
        if constexpr (is_ssl_socket_v<SocketType>) {
#ifdef GALAY_SSL_FEATURE_ENABLED
            return galay::ssl::SslAwaitableBuilder<ResultType>::fromStateMachine(
                       m_socket.controller(),
                       &m_socket,
                       detail::WsSslEchoMachine<SocketType>(this, reader_setting, writer_setting, message, opcode))
                .build();
#else
            static_assert(!sizeof(SocketType), "SSL support is disabled");
#endif
        } else {
            return AwaitableBuilder<ResultType>::fromStateMachine(
                       m_socket.controller(),
                       detail::WsEchoMachine<SocketType>(this, reader_setting, writer_setting, message, opcode))
                .build();
        }
    }

    /**
     * @brief 单次回显并允许消费 message 缓冲
     * @details 仅适合调用方在返回后不再依赖 text/binary payload 内容的场景。
     */
    auto echoOnceConsume(std::string& message,
                         WsOpcode& opcode,
                         const WsReaderSetting& reader_setting = WsReaderSetting(),
                         WsWriterSetting writer_setting = WsWriterSetting::byServer()) {
        ++m_echo_counters.composite_awaitables_started;
        using ResultType = std::expected<bool, WsError>;
        if constexpr (is_ssl_socket_v<SocketType>) {
#ifdef GALAY_SSL_FEATURE_ENABLED
            return galay::ssl::SslAwaitableBuilder<ResultType>::fromStateMachine(
                       m_socket.controller(),
                       &m_socket,
                       detail::WsSslEchoMachine<SocketType>(this, reader_setting, writer_setting, message, opcode, false))
                .build();
#else
            static_assert(!sizeof(SocketType), "SSL support is disabled");
#endif
        } else {
            return AwaitableBuilder<ResultType>::fromStateMachine(
                       m_socket.controller(),
                       detail::WsEchoMachine<SocketType>(this, reader_setting, writer_setting, message, opcode, false))
                .build();
        }
    }

#ifdef GALAY_SSL_FEATURE_ENABLED
    auto echoLoopConsume(const WsReaderSetting& reader_setting = WsReaderSetting(),
                         WsWriterSetting writer_setting = WsWriterSetting::byServer()) {
        ++m_echo_counters.composite_awaitables_started;
        using ResultType = std::expected<bool, WsError>;
        if constexpr (is_ssl_socket_v<SocketType>) {
            return galay::ssl::SslAwaitableBuilder<ResultType>::fromStateMachine(
                       m_socket.controller(),
                       &m_socket,
                       detail::WsSslEchoLoopMachine<SocketType>(this, reader_setting, writer_setting))
                .build();
        } else {
            return AwaitableBuilder<ResultType>::fromStateMachine(
                       m_socket.controller(),
                       detail::WsEchoMachine<SocketType>(
                           this,
                           reader_setting,
                           writer_setting,
                           m_loop_message_scratch,
                           m_loop_opcode_scratch,
                           false))
                .build();
        }
    }
#endif

    /**
     * @brief 是否为服务器端连接
     */
    bool isServer() const { return m_is_server; }

    // 允许WsServerImpl访问私有成员
    template<typename S>
    friend class WsServerImpl;
    friend struct detail::WsEchoMachine<SocketType>;
    friend struct detail::WsSslEchoMachine<SocketType>;
#ifdef GALAY_SSL_FEATURE_ENABLED
    friend struct detail::WsSslEchoLoopMachine<SocketType>;
#endif

private:
    SocketType m_socket;
    RingBuffer m_ring_buffer;
    bool m_is_server;
    EchoCounters m_echo_counters;
    std::string m_loop_message_scratch;
    WsOpcode m_loop_opcode_scratch = WsOpcode::Close;
};

// 类型别名 - WebSocket over TCP
using WsConn = WsConnImpl<TcpSocket>;

} // namespace galay::websocket

#ifdef GALAY_SSL_FEATURE_ENABLED
#include "galay-ssl/async/ssl_socket.h"
namespace galay::websocket {
using WssConn = WsConnImpl<galay::ssl::SslSocket>;
} // namespace galay::websocket
#endif

#endif // GALAY_WS_CONN_H
