#include <algorithm>
#include <cstring>
#include <iostream>
#include <string>

#include <sstream>

#define private public
#include "ws/kernel/ws_conn.h"
#undef private

using galay::async::TcpSocket;
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

std::string encodeMaskedFrame(WsOpcode opcode, std::string payload, bool fin = true) {
    WsFrame frame(opcode, std::move(payload), fin);
    std::string encoded;
    WsFrameParser::encodeInto(encoded, frame, true);
    return encoded;
}

bool writeAll(const struct iovec* iovecs, size_t count, std::string_view bytes) {
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

std::string flattenIovecs(const struct iovec* iovecs, size_t count) {
    std::string result;
    for (size_t index = 0; index < count; ++index) {
        result.append(static_cast<const char*>(iovecs[index].iov_base), iovecs[index].iov_len);
    }
    return result;
}

RingBuffer makeWrappedFrameBuffer(std::string_view encoded, size_t capacity = 64, size_t prefix = 40) {
    RingBuffer ring(capacity);
    std::string head(prefix, 'x');
    if (ring.write(head.data(), head.size()) != head.size()) {
        throw std::runtime_error("failed to seed ring prefix");
    }
    ring.consume(prefix - 10);
    if (ring.write(encoded.data(), encoded.size()) != encoded.size()) {
        throw std::runtime_error("failed to wrap encoded frame into ring");
    }
    ring.consume(10);
    return ring;
}

} // namespace

int main() {
    {
        TcpSocket socket;
        WsConn conn(std::move(socket), true);
        auto reader = conn.getReader();
        auto writer = conn.getWriter(WsWriterSetting::byServer());

        std::string message;
        WsOpcode opcode = WsOpcode::Close;
        auto read_op = reader.getMessage(message, opcode);
        (void) read_op;
        auto write_op = writer.sendText("split-path");
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
        TcpSocket socket;
        WsConn conn(std::move(socket), true);
        std::string message;
        WsOpcode opcode = WsOpcode::Close;
        auto echo_op = conn.echoOnce(message, opcode);
        (void) echo_op;

        if (!check(conn.m_echo_counters.composite_awaitables_started == 1,
                   "composite path should start one awaitable")) {
            return 1;
        }
    }

    {
        TcpSocket socket;
        WsConn conn(std::move(socket), true);
        std::string message;
        WsOpcode opcode = WsOpcode::Close;
        galay::websocket::detail::WsEchoMachine<TcpSocket> machine(
            &conn,
            WsReaderSetting(),
            WsWriterSetting::byServer(),
            message,
            opcode);

        const auto first = machine.advance();
        if (!check(first.signal == MachineSignal::kWaitReadv,
                   "composite text path should wait for readv first")) {
            return 1;
        }

        const std::string payload = "hello composite echo";
        const auto encoded = encodeMaskedFrame(WsOpcode::Text, payload);
        if (!check(writeAll(first.iovecs, first.iov_count, encoded),
                   "failed to copy masked text frame into borrowed read iovecs")) {
            return 1;
        }

        machine.onRead(std::expected<size_t, galay::kernel::IOError>(encoded.size()));
        const auto second = machine.advance();
        if (!check(second.signal == MachineSignal::kWaitWritev,
                   "composite text path should go directly from read completion to writev")) {
            return 1;
        }

        const auto expected_text = WsFrameParser::toBytes(WsFrameParser::createTextFrame(payload), false);
        if (!check(flattenIovecs(second.iovecs, second.iov_count) == expected_text,
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

        machine.onWrite(std::expected<size_t, galay::kernel::IOError>(expected_text.size()));
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
        TcpSocket socket;
        WsConn conn(std::move(socket), true);
        std::string message;
        WsOpcode opcode = WsOpcode::Close;
        auto echo_op = conn.echoOnceConsume(message, opcode);
        (void) echo_op;
        if (!check(conn.m_echo_counters.composite_awaitables_started == 1,
                   "consume composite path should also start one awaitable")) {
            return 1;
        }
    }

    {
        TcpSocket socket;
        WsConn conn(std::move(socket), true);
        std::string message;
        WsOpcode opcode = WsOpcode::Close;
        galay::websocket::detail::WsEchoMachine<TcpSocket> machine(
            &conn,
            WsReaderSetting(),
            WsWriterSetting::byServer(),
            message,
            opcode,
            false);

        const auto first = machine.advance();
        if (!check(first.signal == MachineSignal::kWaitReadv,
                   "consume composite text path should wait for readv first")) {
            return 1;
        }

        const std::string payload = "consume composite echo";
        const auto encoded = encodeMaskedFrame(WsOpcode::Text, payload);
        if (!check(writeAll(first.iovecs, first.iov_count, encoded),
                   "failed to copy masked text frame into consume machine read iovecs")) {
            return 1;
        }

        machine.onRead(std::expected<size_t, galay::kernel::IOError>(encoded.size()));
        const auto second = machine.advance();
        if (!check(second.signal == MachineSignal::kWaitWritev,
                   "consume composite text path should go directly to writev")) {
            return 1;
        }

        const auto expected_text = WsFrameParser::toBytes(WsFrameParser::createTextFrame(payload), false);
        if (!check(flattenIovecs(second.iovecs, second.iov_count) == expected_text,
                   "consume composite text write layout mismatch")) {
            return 1;
        }
        if (!check(conn.m_echo_counters.zero_copy_hits == 1,
                   "consume composite text path should count one zero-copy hit")) {
            return 1;
        }
    }

    {
        TcpSocket socket;
        const std::string payload = "wrapped-zero-copy-hit";
        auto ring = makeWrappedFrameBuffer(encodeMaskedFrame(WsOpcode::Text, payload));
        WsConn conn(std::move(socket), std::move(ring), true);
        std::string message;
        WsOpcode opcode = WsOpcode::Close;
        galay::websocket::detail::WsEchoMachine<TcpSocket> machine(
            &conn,
            WsReaderSetting(),
            WsWriterSetting::byServer(),
            message,
            opcode,
            false);

        const auto first = machine.advance();
        if (!check(first.signal == MachineSignal::kWaitWritev,
                   "wrapped consume path should still enter writev immediately")) {
            return 1;
        }

        const auto expected_text = WsFrameParser::toBytes(WsFrameParser::createTextFrame(payload), false);
        if (!check(flattenIovecs(first.iovecs, first.iov_count) == expected_text,
                   "wrapped consume text write layout mismatch")) {
            return 1;
        }
        if (!check(conn.m_echo_counters.zero_copy_hits == 1,
                   "wrapped consume text path should count one zero-copy hit")) {
            return 1;
        }
    }

    {
        TcpSocket socket;
        WsConn conn(std::move(socket), true);
        std::string message;
        WsOpcode opcode = WsOpcode::Close;
        auto machine = galay::websocket::detail::WsEchoMachine<TcpSocket>(
            &conn,
            WsReaderSetting(),
            WsWriterSetting::byServer(),
            message,
            opcode);
        auto moved = std::move(machine);

        const auto first = moved.advance();
        if (!check(first.signal == MachineSignal::kWaitReadv,
                   "moved composite machine should still wait for readv first")) {
            return 1;
        }

        const std::string payload = "moved machine";
        const auto encoded = encodeMaskedFrame(WsOpcode::Text, payload);
        if (!check(writeAll(first.iovecs, first.iov_count, encoded),
                   "failed to copy masked frame into moved machine read iovecs")) {
            return 1;
        }

        moved.onRead(std::expected<size_t, galay::kernel::IOError>(encoded.size()));
        const auto second = moved.advance();
        if (!check(second.signal == MachineSignal::kWaitWritev,
                   "moved composite machine should still transition to writev")) {
            return 1;
        }

        const auto expected_text = WsFrameParser::toBytes(WsFrameParser::createTextFrame(payload), false);
        moved.onWrite(std::expected<size_t, galay::kernel::IOError>(expected_text.size()));
        const auto third = moved.advance();
        if (!check(third.signal == MachineSignal::kComplete &&
                       third.result.has_value() && third.result->has_value() && third.result->value(),
                   "moved composite machine should still complete successfully")) {
            return 1;
        }
    }

    {
        TcpSocket socket;
        WsConn conn(std::move(socket), true);
        std::string message;
        WsOpcode opcode = WsOpcode::Close;
        galay::websocket::detail::WsEchoMachine<TcpSocket> machine(
            &conn,
            WsReaderSetting(),
            WsWriterSetting::byServer(),
            message,
            opcode);

        const auto first = machine.advance();
        if (!check(first.signal == MachineSignal::kWaitReadv,
                   "composite ping path should wait for readv first")) {
            return 1;
        }

        const auto encoded = encodeMaskedFrame(WsOpcode::Ping, "ping");
        if (!check(writeAll(first.iovecs, first.iov_count, encoded),
                   "failed to copy masked ping frame into borrowed read iovecs")) {
            return 1;
        }

        machine.onRead(std::expected<size_t, galay::kernel::IOError>(encoded.size()));
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
