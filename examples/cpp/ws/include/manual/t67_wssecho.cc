#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#ifdef GALAY_SSL_FEATURE_ENABLED
#include <sstream>

#define private public
#include <galay/cpp/galay-ws/kernel/ws_conn.h>
#undef private
#include <galay/cpp/galay-ws/protoc/ws_frame.h>
#include <galay/cpp/galay-utils/buffer/bytes.hpp>
#include <galay/cpp/galay-utils/buffer/ring_buffer.hpp>
#include <galay/cpp/galay-ssl/async/ssl_socket.h>
#endif

namespace {

bool check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "[T67] " << message << "\n";
        return false;
    }
    return true;
}

#ifdef GALAY_SSL_FEATURE_ENABLED
std::string encode_masked_frame(galay::websocket::WsOpcode opcode, std::string payload, bool fin = true) {
    galay::websocket::WsFrame frame(opcode, std::move(payload), fin);
    std::string encoded;
    galay::websocket::WsFrameParser::encode_into(encoded, frame, true);
    return encoded;
}

galay::utils::RingBuffer<galay::utils::RingBufferBackendStrategy::Mmap, std::dynamic_extent> make_wrapped_frame_buffer(std::string_view encoded, size_t capacity = 64, size_t prefix = 40) {
    galay::utils::RingBuffer<galay::utils::RingBufferBackendStrategy::Mmap, std::dynamic_extent> ring(capacity);
    std::string head(prefix, 'x');
    if (ring.try_write_batch(head.data(), head.size()) != head.size()) {
        throw std::runtime_error("failed to seed ring prefix");
    }
    ring.consume(prefix - 10);
    if (ring.try_write_batch(encoded.data(), encoded.size()) != encoded.size()) {
        throw std::runtime_error("failed to wrap encoded frame into ring");
    }
    ring.consume(10);
    return ring;
}

template<typename MachineT, typename ActionT>
std::string drain_ssl_send(MachineT& machine, ActionT action) {
    std::string sent;
    while (action.signal == galay::ssl::SslMachineSignal::kSend) {
        sent.append(action.write_buffer, action.write_length);
        machine.on_send(std::expected<size_t, galay::ssl::SslError>(action.write_length));
        action = machine.advance();
    }
    return sent;
}
#endif

} // namespace

int main() {
#ifndef GALAY_SSL_FEATURE_ENABLED
    std::cout << "T67-WssEchoCompositeSsl SKIP\n";
    return 0;
#else
    using namespace galay::websocket;

    {
        galay::ssl::SslSocket socket(nullptr);
        WssConn conn(std::move(socket), true);
        auto loop = conn.echo_loop_consume();
        (void) loop;
        if (!check(conn.m_echo_counters.composite_awaitables_started == 1,
                   "ssl echo loop consume should start one awaitable")) {
            return 1;
        }
    }

    {
        galay::ssl::SslSocket socket(nullptr);
        galay::utils::RingBuffer<galay::utils::RingBufferBackendStrategy::Mmap, std::dynamic_extent> ring(256);
        const auto text1 = encode_masked_frame(WsOpcode::Text, "loop-one");
        const auto text2 = encode_masked_frame(WsOpcode::Text, "loop-two");
        const auto close = encode_masked_frame(WsOpcode::Close, "");
        const std::string encoded = text1 + text2 + close;
        if (ring.try_write_batch(encoded.data(), encoded.size()) != encoded.size()) {
            std::cerr << "[T67] failed to seed loop ring buffer\n";
            return 1;
        }

        WssConn conn(std::move(socket), std::move(ring), true);
        galay::websocket::detail::WsSslEchoLoopMachine<galay::ssl::SslSocket> machine(
            &conn,
            WsReaderSetting(),
            WsWriterSetting::by_server());

        std::string sent_stream;
        auto action = machine.advance();
        while (action.signal != galay::ssl::SslMachineSignal::kComplete) {
            if (!check(action.signal == galay::ssl::SslMachineSignal::kSend,
                       "ssl echo loop should keep sending buffered frames")) {
                return 1;
            }

            while (action.signal == galay::ssl::SslMachineSignal::kSend) {
                sent_stream.append(action.write_buffer, action.write_length);
                machine.on_send(std::expected<size_t, galay::ssl::SslError>(action.write_length));
                action = machine.advance();
            }
        }

        if (!check(action.result.has_value() && action.result->has_value() && action.result->value(),
                   "ssl echo loop should complete successfully")) {
            return 1;
        }
        const std::string expected_stream =
            WsFrameParser::to_bytes(WsFrameParser::create_text_frame("loop-one"), false) +
            WsFrameParser::to_bytes(WsFrameParser::create_text_frame("loop-two"), false) +
            WsFrameParser::to_bytes(WsFrameParser::create_close_frame(WsCloseCode::Normal), false);
        if (!check(sent_stream == expected_stream,
                   "ssl echo loop output stream mismatch")) {
            return 1;
        }
        if (!check(conn.m_echo_counters.zero_copy_hits == 2,
                   "ssl echo loop should keep zero-copy hits across multiple messages")) {
            return 1;
        }
    }

    {
        galay::ssl::SslSocket socket(nullptr);
        WssConn conn(std::move(socket), true);
        std::string message;
        WsOpcode opcode = WsOpcode::Close;
        galay::websocket::detail::WsSslEchoMachine<galay::ssl::SslSocket> machine(
            &conn,
            WsReaderSetting(),
            WsWriterSetting::by_server(),
            message,
            opcode);

        const auto first = machine.advance();
        if (!check(first.signal == galay::ssl::SslMachineSignal::kRecv,
                   "ssl composite echo should recv first")) {
            return 1;
        }

        const std::string payload = "hello ssl composite";
        const auto encoded = encode_masked_frame(WsOpcode::Text, payload);
        if (!check(first.read_length >= encoded.size(),
                   "ssl composite recv window should fit encoded frame")) {
            return 1;
        }
        std::memcpy(first.read_buffer, encoded.data(), encoded.size());

        machine.on_recv(std::expected<galay::utils::Bytes, galay::ssl::SslError>(
            galay::utils::Bytes(first.read_buffer, encoded.size())));

        const auto second = machine.advance();
        if (!check(second.signal == galay::ssl::SslMachineSignal::kSend,
                   "ssl composite echo should send immediately after parse")) {
            return 1;
        }

        const auto expected = WsFrameParser::to_bytes(WsFrameParser::create_text_frame(payload), false);
        if (!check(second.write_length == expected.size(),
                   "ssl composite echo send length mismatch")) {
            return 1;
        }
        if (!check(std::string(second.write_buffer, second.write_length) == expected,
                   "ssl composite echo send payload mismatch")) {
            return 1;
        }
        if (!check(conn.m_echo_counters.composite_hits == 1,
                   "ssl composite echo should count one hit")) {
            return 1;
        }
        if (!check(conn.m_echo_counters.ssl_direct_message_hits == 1,
                   "ssl composite echo should use direct message encoding")) {
            return 1;
        }
        if (!check(conn.m_echo_counters.zero_copy_hits == 0,
                   "ssl preserve composite echo should not count zero-copy hit")) {
            return 1;
        }

        machine.on_send(std::expected<size_t, galay::ssl::SslError>(second.write_length));
        const auto third = machine.advance();
        if (!check(third.signal == galay::ssl::SslMachineSignal::kComplete,
                   "ssl composite echo should complete after send")) {
            return 1;
        }
        if (!check(third.result.has_value() && third.result->has_value() && third.result->value(),
                   "ssl composite echo should complete successfully")) {
            return 1;
        }
        if (!check(message == payload, "ssl composite echo should preserve message")) {
            return 1;
        }
    }

    {
        galay::ssl::SslSocket socket(nullptr);
        WssConn conn(std::move(socket), true);
        std::string message;
        WsOpcode opcode = WsOpcode::Close;
        auto echo_op = conn.echo_once_consume(message, opcode);
        (void) echo_op;
        if (!check(conn.m_echo_counters.composite_awaitables_started == 1,
                   "ssl consume composite path should start one awaitable")) {
            return 1;
        }
    }

    {
        galay::ssl::SslSocket socket(nullptr);
        WssConn conn(std::move(socket), true);
        std::string message;
        WsOpcode opcode = WsOpcode::Close;
        galay::websocket::detail::WsSslEchoMachine<galay::ssl::SslSocket> machine(
            &conn,
            WsReaderSetting(),
            WsWriterSetting::by_server(),
            message,
            opcode,
            false);

        const auto first = machine.advance();
        if (!check(first.signal == galay::ssl::SslMachineSignal::kRecv,
                   "ssl consume composite echo should recv first")) {
            return 1;
        }

        const std::string payload = "consume ssl composite";
        const auto encoded = encode_masked_frame(WsOpcode::Text, payload);
        if (!check(first.read_length >= encoded.size(),
                   "ssl consume composite recv window should fit encoded frame")) {
            return 1;
        }
        std::memcpy(first.read_buffer, encoded.data(), encoded.size());
        machine.on_recv(std::expected<galay::utils::Bytes, galay::ssl::SslError>(
            galay::utils::Bytes(first.read_buffer, encoded.size())));

        const auto second = machine.advance();
        if (!check(second.signal == galay::ssl::SslMachineSignal::kSend,
                   "ssl consume composite echo should send after parse")) {
            return 1;
        }

        const auto expected = WsFrameParser::to_bytes(WsFrameParser::create_text_frame(payload), false);
        auto send_action = second;
        const auto sent = drain_ssl_send(machine, send_action);
        if (!check(sent == expected,
                   "ssl consume composite send payload mismatch")) {
            return 1;
        }
        if (!check(conn.m_echo_counters.zero_copy_hits == 1,
                   "ssl consume composite echo should count one zero-copy hit")) {
            return 1;
        }
    }

    {
        galay::ssl::SslSocket socket(nullptr);
        const std::string payload = "wrapped ssl zero-copy";
        auto ring = make_wrapped_frame_buffer(encode_masked_frame(WsOpcode::Text, payload));
        WssConn conn(std::move(socket), std::move(ring), true);
        std::string message;
        WsOpcode opcode = WsOpcode::Close;
        galay::websocket::detail::WsSslEchoMachine<galay::ssl::SslSocket> machine(
            &conn,
            WsReaderSetting(),
            WsWriterSetting::by_server(),
            message,
            opcode,
            false);

        const auto first = machine.advance();
        if (!check(first.signal == galay::ssl::SslMachineSignal::kSend,
                   "wrapped ssl consume path should send immediately")) {
            return 1;
        }

        const auto expected = WsFrameParser::to_bytes(WsFrameParser::create_text_frame(payload), false);
        auto send_action = first;
        const auto sent = drain_ssl_send(machine, send_action);
        if (!check(sent == expected,
                   "wrapped ssl consume send payload mismatch")) {
            return 1;
        }
        if (!check(conn.m_echo_counters.zero_copy_hits == 1,
                   "wrapped ssl consume path should count one zero-copy hit")) {
            return 1;
        }
    }

    {
        galay::ssl::SslSocket socket(nullptr);
        WssConn conn(std::move(socket), true);
        std::string message;
        WsOpcode opcode = WsOpcode::Close;
        galay::websocket::detail::WsSslEchoMachine<galay::ssl::SslSocket> machine(
            &conn,
            WsReaderSetting(),
            WsWriterSetting::by_server(),
            message,
            opcode);

        const auto first = machine.advance();
        if (!check(first.signal == galay::ssl::SslMachineSignal::kRecv,
                   "ssl composite ping should recv first")) {
            return 1;
        }

        const auto encoded = encode_masked_frame(WsOpcode::Ping, "ping");
        std::memcpy(first.read_buffer, encoded.data(), encoded.size());
        machine.on_recv(std::expected<galay::utils::Bytes, galay::ssl::SslError>(
            galay::utils::Bytes(first.read_buffer, encoded.size())));

        const auto second = machine.advance();
        if (!check(second.signal == galay::ssl::SslMachineSignal::kComplete,
                   "ssl composite ping should stay on fallback path")) {
            return 1;
        }
        if (!check(opcode == WsOpcode::Ping, "ssl composite ping should surface opcode")) {
            return 1;
        }
        if (!check(conn.m_echo_counters.composite_hits == 0,
                   "ssl composite ping should not count hit")) {
            return 1;
        }
        if (!check(conn.m_echo_counters.composite_fallbacks == 1,
                   "ssl composite ping should count fallback")) {
            return 1;
        }
    }

    std::cout << "T67-WssEchoCompositeSsl PASS\n";
    return 0;
#endif
}
