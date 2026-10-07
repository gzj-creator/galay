#include <algorithm>
#include <cstring>
#include <iostream>
#include <string>

#include <sstream>

#define private public
#include <galay/cpp/galay-ws/kernel/ws_conn.h>
#undef private

using galay::async::AsyncTcpSocket;
using galay::kernel::MachineSignal;
using galay::utils::RingBuffer;
using namespace galay::websocket;

namespace {

bool check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "[T62] " << message << "\n";
        return false;
    }
    return true;
}

std::string encode_masked_frame(WsOpcode opcode, std::string payload, bool fin = true) {
    WsFrame frame(opcode, std::move(payload), fin);
    std::string encoded;
    WsFrameParser::encode_into(encoded, frame, true);
    return encoded;
}

bool write_all(const struct iovec* iovecs, size_t count, std::string_view bytes) {
    size_t offset = 0;
    for (size_t index = 0; index < count && offset < bytes.size(); ++index) {
        const size_t take = std::min(iovecs[index].iov_len, bytes.size() - offset);
        if (take == 0) {
            continue;
        }
        std::memcpy(iovecs[index].iov_base, bytes.data() + offset, take);
        offset += take;
    }
    return offset == bytes.size();
}

std::string flatten_iovecs(const struct iovec* iovecs, size_t count) {
    std::string result;
    for (size_t index = 0; index < count; ++index) {
        result.append(static_cast<const char*>(iovecs[index].iov_base), iovecs[index].iov_len);
    }
    return result;
}

RingBuffer<galay::utils::RingBufferBackendStrategy::Mmap, std::dynamic_extent> make_wrapped_frame_buffer(std::string_view encoded, size_t capacity = 64, size_t prefix = 40) {
    RingBuffer ring(capacity);
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

} // namespace

int main() {
    {
        AsyncTcpSocket socket;
        WsConn conn(std::move(socket), true);
        auto reader = conn.get_reader();
        auto writer = conn.get_writer(WsWriterSetting::by_server());

        std::string message;
        WsOpcode opcode = WsOpcode::Close;
        auto read_op = reader.get_message(message, opcode);
        (void) read_op;
        auto write_op = writer.send_text("split-path");
        (void) write_op;

        if (!check(reader.m_operation_counters.message_awaitables_started == 1,
                   "split path should start one message awaitable")) {
            return 1;
        }
        if (!check(writer.m_operation_counters.send_awaitables_started == 1,
                   "split path should start one send awaitable")) {
            return 1;
        }
        if (!check(reader.m_operation_counters.message_awaitables_started +
                       writer.m_operation_counters.send_awaitables_started == 2,
                   "split path should require two awaitable starts")) {
            return 1;
        }
    }

    {
        AsyncTcpSocket socket;
        WsConn conn(std::move(socket), true);
        std::string message;
        WsOpcode opcode = WsOpcode::Close;
        auto echo_op = conn.echo_once(message, opcode);
        (void) echo_op;

        if (!check(conn.m_echo_counters.composite_awaitables_started == 1,
                   "composite path should start one awaitable")) {
            return 1;
        }
    }

    {
        AsyncTcpSocket socket;
        WsConn conn(std::move(socket), true);
        std::string message;
        WsOpcode opcode = WsOpcode::Close;
        galay::websocket::detail::WsEchoMachine<AsyncTcpSocket> machine(
            &conn,
            WsReaderSetting(),
            WsWriterSetting::by_server(),
            message,
            opcode);

        const auto first = machine.advance();
        if (!check(first.signal == MachineSignal::kWaitReadv,
                   "composite text path should wait for readv first")) {
            return 1;
        }

        const std::string payload = "hello composite echo";
        const auto encoded = encode_masked_frame(WsOpcode::Text, payload);
        if (!check(write_all(first.iovecs, first.iov_count, encoded),
                   "failed to copy masked text frame into borrowed read iovecs")) {
            return 1;
        }

        machine.on_read(std::expected<size_t, galay::kernel::IOError>(encoded.size()));
        const auto second = machine.advance();
        if (!check(second.signal == MachineSignal::kWaitWritev,
                   "composite text path should go directly from read completion to writev")) {
            return 1;
        }

        const auto expected_text = WsFrameParser::to_bytes(WsFrameParser::create_text_frame(payload), false);
        if (!check(flatten_iovecs(second.iovecs, second.iov_count) == expected_text,
                   "composite text write layout mismatch")) {
            return 1;
        }
        if (!check(conn.m_echo_counters.composite_hits == 1,
                   "composite text path should count one hit")) {
            return 1;
        }
        if (!check(conn.m_echo_counters.zero_copy_hits == 0,
                   "preserve composite text path should not count zero-copy hit")) {
            return 1;
        }

        machine.on_write(std::expected<size_t, galay::kernel::IOError>(expected_text.size()));
        const auto third = machine.advance();
        if (!check(third.signal == MachineSignal::kComplete,
                   "composite text path should complete after write")) {
            return 1;
        }
        if (!check(third.result.has_value() && third.result->has_value() && third.result->value(),
                   "composite text path should complete successfully")) {
            return 1;
        }
        if (!check(message == payload,
                   "composite text path should preserve caller-visible message content")) {
            return 1;
        }
    }

    {
        AsyncTcpSocket socket;
        WsConn conn(std::move(socket), true);
        std::string message;
        WsOpcode opcode = WsOpcode::Close;
        auto echo_op = conn.echo_once_consume(message, opcode);
        (void) echo_op;
        if (!check(conn.m_echo_counters.composite_awaitables_started == 1,
                   "consume composite path should also start one awaitable")) {
            return 1;
        }
    }

    {
        AsyncTcpSocket socket;
        WsConn conn(std::move(socket), true);
        std::string message;
        WsOpcode opcode = WsOpcode::Close;
        galay::websocket::detail::WsEchoMachine<AsyncTcpSocket> machine(
            &conn,
            WsReaderSetting(),
            WsWriterSetting::by_server(),
            message,
            opcode,
            false);

        const auto first = machine.advance();
        if (!check(first.signal == MachineSignal::kWaitReadv,
                   "consume composite text path should wait for readv first")) {
            return 1;
        }

        const std::string payload = "consume composite echo";
        const auto encoded = encode_masked_frame(WsOpcode::Text, payload);
        if (!check(write_all(first.iovecs, first.iov_count, encoded),
                   "failed to copy masked text frame into consume machine read iovecs")) {
            return 1;
        }

        machine.on_read(std::expected<size_t, galay::kernel::IOError>(encoded.size()));
        const auto second = machine.advance();
        if (!check(second.signal == MachineSignal::kWaitWritev,
                   "consume composite text path should go directly to writev")) {
            return 1;
        }

        const auto expected_text = WsFrameParser::to_bytes(WsFrameParser::create_text_frame(payload), false);
        if (!check(flatten_iovecs(second.iovecs, second.iov_count) == expected_text,
                   "consume composite text write layout mismatch")) {
            return 1;
        }
        if (!check(conn.m_echo_counters.zero_copy_hits == 1,
                   "consume composite text path should count one zero-copy hit")) {
            return 1;
        }
    }

    {
        AsyncTcpSocket socket;
        const std::string payload = "wrapped-zero-copy-hit";
        auto ring = make_wrapped_frame_buffer(encode_masked_frame(WsOpcode::Text, payload));
        WsConn conn(std::move(socket), std::move(ring), true);
        std::string message;
        WsOpcode opcode = WsOpcode::Close;
        galay::websocket::detail::WsEchoMachine<AsyncTcpSocket> machine(
            &conn,
            WsReaderSetting(),
            WsWriterSetting::by_server(),
            message,
            opcode,
            false);

        const auto first = machine.advance();
        if (!check(first.signal == MachineSignal::kWaitWritev,
                   "wrapped consume path should still enter writev immediately")) {
            return 1;
        }

        const auto expected_text = WsFrameParser::to_bytes(WsFrameParser::create_text_frame(payload), false);
        if (!check(flatten_iovecs(first.iovecs, first.iov_count) == expected_text,
                   "wrapped consume text write layout mismatch")) {
            return 1;
        }
        if (!check(conn.m_echo_counters.zero_copy_hits == 1,
                   "wrapped consume text path should count one zero-copy hit")) {
            return 1;
        }
    }

    {
        AsyncTcpSocket socket;
        WsConn conn(std::move(socket), true);
        std::string message;
        WsOpcode opcode = WsOpcode::Close;
        auto machine = galay::websocket::detail::WsEchoMachine<AsyncTcpSocket>(
            &conn,
            WsReaderSetting(),
            WsWriterSetting::by_server(),
            message,
            opcode);
        auto moved = std::move(machine);

        const auto first = moved.advance();
        if (!check(first.signal == MachineSignal::kWaitReadv,
                   "moved composite machine should still wait for readv first")) {
            return 1;
        }

        const std::string payload = "moved machine";
        const auto encoded = encode_masked_frame(WsOpcode::Text, payload);
        if (!check(write_all(first.iovecs, first.iov_count, encoded),
                   "failed to copy masked frame into moved machine read iovecs")) {
            return 1;
        }

        moved.on_read(std::expected<size_t, galay::kernel::IOError>(encoded.size()));
        const auto second = moved.advance();
        if (!check(second.signal == MachineSignal::kWaitWritev,
                   "moved composite machine should still transition to writev")) {
            return 1;
        }

        const auto expected_text = WsFrameParser::to_bytes(WsFrameParser::create_text_frame(payload), false);
        moved.on_write(std::expected<size_t, galay::kernel::IOError>(expected_text.size()));
        const auto third = moved.advance();
        if (!check(third.signal == MachineSignal::kComplete &&
                       third.result.has_value() && third.result->has_value() && third.result->value(),
                   "moved composite machine should still complete successfully")) {
            return 1;
        }
    }

    {
        AsyncTcpSocket socket;
        WsConn conn(std::move(socket), true);
        std::string message;
        WsOpcode opcode = WsOpcode::Close;
        galay::websocket::detail::WsEchoMachine<AsyncTcpSocket> machine(
            &conn,
            WsReaderSetting(),
            WsWriterSetting::by_server(),
            message,
            opcode);

        const auto first = machine.advance();
        if (!check(first.signal == MachineSignal::kWaitReadv,
                   "composite ping path should wait for readv first")) {
            return 1;
        }

        const auto encoded = encode_masked_frame(WsOpcode::Ping, "ping");
        if (!check(write_all(first.iovecs, first.iov_count, encoded),
                   "failed to copy masked ping frame into borrowed read iovecs")) {
            return 1;
        }

        machine.on_read(std::expected<size_t, galay::kernel::IOError>(encoded.size()));
        const auto second = machine.advance();
        if (!check(second.signal == MachineSignal::kComplete,
                   "control frame should stay on fallback path and complete without writev")) {
            return 1;
        }
        if (!check(opcode == WsOpcode::Ping,
                   "control frame fallback should surface ping opcode")) {
            return 1;
        }
        if (!check(conn.m_echo_counters.composite_hits == 0,
                   "control frame should not count as composite hit")) {
            return 1;
        }
        if (!check(conn.m_echo_counters.composite_fallbacks == 1,
                   "control frame should count as composite fallback")) {
            return 1;
        }
    }

    std::cout << "T62-WsEchoFewerWakeups PASS\n";
    return 0;
}
