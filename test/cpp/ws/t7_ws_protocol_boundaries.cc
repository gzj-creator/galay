#include <algorithm>
#include <array>
#include <cstring>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/uio.h>
#include <vector>

#include <galay/cpp/galay-http/protoc/http_request.h>

#define private public
#include <galay/cpp/galay-ws/kernel/ws_conn.h>
#include <galay/cpp/galay-ws/server/ws_upgrade.h>
#undef private

using galay::async::TcpSocket;
using galay::kernel::MachineSignal;
using galay::utils::RingBuffer;
using namespace galay::http;
using namespace galay::websocket;

namespace {

[[noreturn]] void fail(const std::string& message)
{
    std::cerr << "[T7] " << message << "\n";
    std::abort();
}

void check(bool condition, const std::string& message)
{
    if (!condition) {
        fail(message);
    }
}

std::string makeMaskedRaw(WsOpcode opcode,
                          uint8_t length_code,
                          std::vector<uint8_t> extended_length,
                          std::string payload,
                          bool fin = true)
{
    const std::array<uint8_t, 4> key{0x01, 0x02, 0x03, 0x04};
    std::string out;
    out.push_back(static_cast<char>((fin ? 0x80 : 0x00) | (static_cast<uint8_t>(opcode) & 0x0F)));
    out.push_back(static_cast<char>(0x80 | length_code));
    for (uint8_t byte : extended_length) {
        out.push_back(static_cast<char>(byte));
    }
    for (uint8_t byte : key) {
        out.push_back(static_cast<char>(byte));
    }
    for (size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<char>(static_cast<uint8_t>(payload[i]) ^ key[i % key.size()]);
    }
    out += payload;
    return out;
}

std::string encodeMaskedFrame(WsOpcode opcode, std::string payload, bool fin = true)
{
    WsFrame frame(opcode, std::move(payload), fin);
    std::string encoded;
    WsFrameParser::encodeInto(encoded, frame, true);
    return encoded;
}

void writeAll(RingBuffer& ring, std::string_view bytes)
{
    const size_t written = ring.write(bytes.data(), bytes.size());
    check(written == bytes.size(), "ring buffer write truncated");
}

void expectFrameError(const std::string& raw, WsErrorCode code, const std::string& label)
{
    std::string mutable_raw = raw;
    iovec iov{
        .iov_base = mutable_raw.data(),
        .iov_len = mutable_raw.size(),
    };
    WsFrame frame;
    auto parsed = WsFrameParser::fromIOVec(&iov, 1, frame, true);
    check(!parsed.has_value(), label + " should fail");
    check(parsed.error().code() == code,
          label + " expected error " + std::to_string(code) +
              " but got " + std::to_string(parsed.error().code()));
}

HttpRequest parseUpgradeRequest(const std::string& key)
{
    std::string raw =
        "GET /chat HTTP/1.1\r\n"
        "Host: example.com\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "Sec-WebSocket-Key: " + key + "\r\n"
        "\r\n";
    iovec iov{
        .iov_base = raw.data(),
        .iov_len = raw.size(),
    };
    HttpRequest request;
    const auto [err, consumed] = request.fromIOVec({iov});
    check(err == kNoError && consumed == static_cast<ssize_t>(raw.size()),
          "upgrade request fixture should parse");
    return request;
}

void testFrameLengthEncodingBoundaries()
{
    expectFrameError(
        makeMaskedRaw(WsOpcode::Binary, 126, {0x00, 0x7D}, std::string(125, 'a')),
        kWsInvalidPayloadLength,
        "non-minimal 16-bit length");

    expectFrameError(
        makeMaskedRaw(WsOpcode::Binary, 127, {0, 0, 0, 0, 0, 0, 0, 126}, std::string(126, 'b')),
        kWsInvalidPayloadLength,
        "non-minimal 64-bit length");

    expectFrameError(
        makeMaskedRaw(WsOpcode::Binary, 127, {0x80, 0, 0, 0, 0, 0, 0, 0}, ""),
        kWsInvalidPayloadLength,
        "64-bit length with MSB set");
}

void testCloseFrameValidation()
{
    expectFrameError(
        makeMaskedRaw(WsOpcode::Close, 1, {}, std::string(1, '\0')),
        kWsInvalidPayloadLength,
        "close payload length 1");

    const std::string illegal_code = std::string("\x03\xE7", 2);
    expectFrameError(
        makeMaskedRaw(WsOpcode::Close, 2, {}, illegal_code),
        kWsInvalidCloseCode,
        "illegal close code 999");

    const std::string invalid_reason = std::string("\x03\xE8", 2) + std::string("\xFF", 1);
    expectFrameError(
        makeMaskedRaw(WsOpcode::Close, 3, {}, invalid_reason),
        kWsInvalidUtf8,
        "invalid UTF-8 close reason");
}

void testFragmentedTextValidatesWholeUtf8()
{
    RingBuffer ring(1024);
    std::string buffered;
    buffered += encodeMaskedFrame(WsOpcode::Text, std::string("\xC3", 1), false);
    buffered += encodeMaskedFrame(WsOpcode::Continuation, std::string("(", 1), true);
    writeAll(ring, buffered);

    WsReaderSetting setting;
    setting.max_frame_size = 1024;
    setting.max_message_size = 1024;

    std::string message;
    WsOpcode opcode = WsOpcode::Close;
    galay::websocket::detail::WsMessageReadState state(
        ring, setting, message, opcode, true, false, nullptr);

    check(state.parseFromBuffer(), "fragmented invalid UTF-8 should complete with an error");
    const auto result = state.takeResult();
    check(!result.has_value(), "fragmented invalid UTF-8 should not parse successfully");
    check(result.error().code() == kWsInvalidUtf8,
          "fragmented invalid UTF-8 should return kWsInvalidUtf8");
}

void testEchoZeroCopyHonorsReaderLimits()
{
    TcpSocket socket;
    auto ring = RingBuffer(1024);
    writeAll(ring, encodeMaskedFrame(WsOpcode::Text, "12345"));
    WsConn conn(std::move(socket), std::move(ring), true);

    WsReaderSetting reader_setting;
    reader_setting.max_frame_size = 4;
    reader_setting.max_message_size = 4;

    std::string message;
    WsOpcode opcode = WsOpcode::Close;
    galay::websocket::detail::WsEchoMachine<TcpSocket> machine(
        &conn,
        reader_setting,
        WsWriterSetting::byServer(),
        message,
        opcode,
        false);

    const auto action = machine.advance();
    check(action.signal == MachineSignal::kComplete,
          "oversized zero-copy echo should complete with an error, not waitWritev");
    check(action.result.has_value(), "oversized zero-copy echo should carry a result");
    check(!action.result->has_value(), "oversized zero-copy echo should fail");
    check(action.result->error().code() == kWsMessageTooLarge,
          "oversized zero-copy echo should return kWsMessageTooLarge");
    check(conn.m_echo_counters.zero_copy_hits == 0,
          "oversized zero-copy echo should not count a zero-copy hit");
}

void testUpgradeKeyMustDecodeTo16Bytes()
{
    auto valid = parseUpgradeRequest("dGhlIHNhbXBsZSBub25jZQ==");
    check(WsUpgrade::handleUpgrade(valid).success, "valid 16-byte upgrade key should pass");

    auto short_key = parseUpgradeRequest("YWJj");
    auto short_result = WsUpgrade::handleUpgrade(short_key);
    check(!short_result.success, "upgrade key decoding to 3 bytes should fail");

    auto malformed_key = parseUpgradeRequest("!!!!");
    auto malformed_result = WsUpgrade::handleUpgrade(malformed_key);
    check(!malformed_result.success, "malformed upgrade key should fail");
}

void testUnalignedMaskRoundTrip()
{
    std::array<char, 33> storage{};
    char* unaligned = storage.data() + 1;
    const uint8_t key[4] = {0xA5, 0x5A, 0x11, 0x22};
    for (size_t i = 0; i < 19; ++i) {
        unaligned[i] = static_cast<char>(i + 1);
    }
    const std::string original(unaligned, 19);

    WsFrameParser::applyMaskBytes(unaligned, 19, key);
    check(std::string(unaligned, 19) != original, "mask should alter unaligned payload");
    WsFrameParser::applyMaskBytes(unaligned, 19, key);
    check(std::string(unaligned, 19) == original, "masking twice should restore unaligned payload");
}

} // namespace

int main()
{
    testFrameLengthEncodingBoundaries();
    testCloseFrameValidation();
    testFragmentedTextValidatesWholeUtf8();
    testEchoZeroCopyHonorsReaderLimits();
    testUpgradeKeyMustDecodeTo16Bytes();
    testUnalignedMaskRoundTrip();

    std::cout << "T7-WsProtocolBoundaries PASS\n";
    return 0;
}
