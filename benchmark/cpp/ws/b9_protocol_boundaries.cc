#include <galay/cpp/galay-http/protoc/http_request.h>
#include <galay/cpp/galay-ws/kernel/ws_reader.h>
#include <galay/cpp/galay-ws/protoc/ws_frame.h>
#include <galay/cpp/galay-ws/server/ws_upgrade.h>

#include <array>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <sys/uio.h>
#include <vector>

using namespace galay::http;
using namespace galay::websocket;

namespace {

void require(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "[benchmark_ws_protocol_boundaries] " << message << "\n";
        std::abort();
    }
}

std::string makeMaskedRaw(WsOpcode opcode,
                          uint8_t length_code,
                          std::vector<uint8_t> extended_length,
                          std::string payload)
{
    const std::array<uint8_t, 4> key{0x01, 0x02, 0x03, 0x04};
    std::string out;
    out.push_back(static_cast<char>(0x80 | (static_cast<uint8_t>(opcode) & 0x0F)));
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

WsErrorCode parseFrameCode(std::string& raw)
{
    iovec iov{
        .iov_base = raw.data(),
        .iov_len = raw.size(),
    };
    WsFrame frame;
    auto parsed = WsFrameParser::fromIOVec(&iov, 1, frame, true);
    return parsed.has_value() ? kWsNoError : parsed.error().code();
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
    auto [err, consumed] = request.fromIOVec({iov});
    require(err == kNoError && consumed == static_cast<ssize_t>(raw.size()),
            "upgrade fixture should parse");
    return request;
}

template <typename Func>
void runBench(const char* name, size_t iterations, Func&& func)
{
    const auto start = std::chrono::steady_clock::now();
    size_t accepted = 0;
    for (size_t i = 0; i < iterations; ++i) {
        accepted += func() ? 1 : 0;
    }
    const auto end = std::chrono::steady_clock::now();
    const double seconds = std::chrono::duration<double>(end - start).count();
    std::cout << name << ": " << iterations << " iterations, "
              << (static_cast<double>(iterations) / seconds) << " ops/s, accepted="
              << accepted << "\n";
}

} // namespace

int main()
{
    constexpr size_t kIterations = 50000;
    std::string non_minimal =
        makeMaskedRaw(WsOpcode::Binary, 126, {0x00, 0x7D}, std::string(125, 'a'));
    std::string invalid_close =
        makeMaskedRaw(WsOpcode::Close, 2, {}, std::string("\x03\xE7", 2));

    require(parseFrameCode(non_minimal) == kWsInvalidPayloadLength,
            "non-minimal length fixture should fail");
    require(parseFrameCode(invalid_close) == kWsInvalidCloseCode,
            "invalid close fixture should fail");

    runBench("BM_RejectNonMinimalLength", kIterations, [&]() {
        return parseFrameCode(non_minimal) == kWsInvalidPayloadLength;
    });
    runBench("BM_RejectInvalidCloseCode", kIterations, [&]() {
        return parseFrameCode(invalid_close) == kWsInvalidCloseCode;
    });
    runBench("BM_UnalignedMaskRoundTrip", kIterations, [&]() {
        std::array<char, 33> storage{};
        char* unaligned = storage.data() + 1;
        const uint8_t key[4] = {0xA5, 0x5A, 0x11, 0x22};
        for (size_t i = 0; i < 19; ++i) {
            unaligned[i] = static_cast<char>(i + 1);
        }
        const std::string original(unaligned, 19);
        WsFrameParser::applyMaskBytes(unaligned, 19, key);
        WsFrameParser::applyMaskBytes(unaligned, 19, key);
        return std::string(unaligned, 19) == original;
    });
    runBench("BM_RejectShortUpgradeKey", kIterations, [&]() {
        auto request = parseUpgradeRequest("YWJj");
        return !WsUpgrade::handleUpgrade(request).success;
    });

    const std::string expected_fragmented =
        std::string(128, 'a') + std::string(128, 'b') + std::string(128, 'c');
    const std::string fragmented_frame =
        encodeMaskedFrame(WsOpcode::Text, std::string(128, 'a'), false) +
        encodeMaskedFrame(WsOpcode::Continuation, std::string(128, 'b'), false) +
        encodeMaskedFrame(WsOpcode::Continuation, std::string(128, 'c'), true);
    WsReaderSetting reader_setting;
    reader_setting.max_frame_size = 1024;
    reader_setting.max_message_size = 2048;
    galay::utils::RingBuffer fragmented_ring(fragmented_frame.size() + 16);
    std::string fragmented_message;
    WsOpcode fragmented_opcode = WsOpcode::Close;
    galay::websocket::detail::WsMessageReadState fragmented_state(
        fragmented_ring,
        reader_setting,
        fragmented_message,
        fragmented_opcode,
        true,
        false,
        nullptr);

    runBench("BM_FragmentedTextAssemblyFastPath", kIterations, [&]() {
        fragmented_state.resetForNextMessage();
        fragmented_opcode = WsOpcode::Close;
        const size_t written =
            fragmented_ring.write(fragmented_frame.data(), fragmented_frame.size());
        if (written != fragmented_frame.size()) {
            return false;
        }
        if (!fragmented_state.parseFromBuffer()) {
            return false;
        }
        auto result = fragmented_state.takeResult();
        return result.has_value() &&
               result.value() &&
               fragmented_opcode == WsOpcode::Text &&
               fragmented_message == expected_fragmented &&
               fragmented_state.m_fast_path_frames == 3 &&
               fragmented_ring.readable() == 0;
    });

    return 0;
}
