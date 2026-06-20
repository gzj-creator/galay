#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <sstream>

#define private public
#include <galay/cpp/galay-ws/kernel/ws_reader.h>
#undef private

#include <galay/cpp/galay-ws/protoc/ws_frame.h>
#include <galay/cpp/galay-utils/cache/ring_buffer.hpp>

using galay::utils::RingBuffer;
using namespace galay::websocket;

namespace {

std::string encodeMaskedFrame(WsOpcode opcode, std::string payload, bool fin = true) {
    WsFrame frame(opcode, std::move(payload), fin);
    std::string encoded;
    WsFrameParser::encodeInto(encoded, frame, true);
    return encoded;
}

void writeAll(RingBuffer& ring, std::string_view bytes) {
    const size_t written = ring.write(bytes.data(), bytes.size());
    if (written != bytes.size()) {
        throw std::runtime_error("ring buffer write truncated");
    }
}

bool expectSuccess(galay::websocket::detail::WsMessageReadState& state, const char* label) {
    const bool completed = state.parseFromBuffer();
    if (!completed) {
        std::cerr << "[T60] " << label << ": parse did not complete\n";
        return false;
    }
    const auto result = state.takeResult();
    if (!result.has_value()) {
        std::cerr << "[T60] " << label << ": unexpected error " << result.error().message() << "\n";
        return false;
    }
    if (!result.value()) {
        std::cerr << "[T60] " << label << ": success result was false\n";
        return false;
    }
    return true;
}

bool expectError(galay::websocket::detail::WsMessageReadState& state, WsErrorCode code, const char* label) {
    const bool completed = state.parseFromBuffer();
    if (!completed) {
        std::cerr << "[T60] " << label << ": parse did not complete\n";
        return false;
    }
    const auto result = state.takeResult();
    if (result.has_value()) {
        std::cerr << "[T60] " << label << ": expected error but got success\n";
        return false;
    }
    if (result.error().code() != code) {
        std::cerr << "[T60] " << label << ": expected error code " << code
                  << " but got " << result.error().code() << "\n";
        return false;
    }
    return true;
}

} // namespace

int main() {
#define CHECK(cond, msg) \
    do { \
        if (!(cond)) { \
            std::cerr << "[T60] " << msg << "\n"; \
            return 1; \
        } \
    } while (false)

    WsReaderSetting setting;
    setting.max_frame_size = 1 << 20;
    setting.max_message_size = 1 << 20;

    {
        RingBuffer ring(1024);
        const std::string payload = "hello websocket";
        writeAll(ring, encodeMaskedFrame(WsOpcode::Text, payload));

        std::string message;
        WsOpcode opcode = WsOpcode::Close;
        galay::websocket::detail::WsMessageReadState state(
            ring, setting, message, opcode, true, false, nullptr);

        CHECK(expectSuccess(state, "single masked text"), "single masked text parse should succeed");
        CHECK(message == payload, "single masked text payload mismatch");
        CHECK(opcode == WsOpcode::Text, "single masked text opcode mismatch");
        CHECK(ring.readable() == 0, "single masked text should consume full frame");
        CHECK(state.m_fast_path_frames > 0, "single masked text frame should hit reader fast path");
    }

    {
        RingBuffer ring(1024);
        const std::string payload("\x01\x02\x03\x04", 4);
        writeAll(ring, encodeMaskedFrame(WsOpcode::Binary, payload));

        std::string message;
        WsOpcode opcode = WsOpcode::Close;
        galay::websocket::detail::WsMessageReadState state(
            ring, setting, message, opcode, true, false, nullptr);

        CHECK(expectSuccess(state, "single masked binary"), "single masked binary parse should succeed");
        CHECK(message == payload, "single masked binary payload mismatch");
        CHECK(opcode == WsOpcode::Binary, "single masked binary opcode mismatch");
        CHECK(ring.readable() == 0, "single masked binary should consume full frame");
        CHECK(state.m_fast_path_frames > 0, "single masked binary frame should hit reader fast path");
    }

    {
        RingBuffer ring(1024);
        std::string buffered;
        buffered += encodeMaskedFrame(WsOpcode::Text, "frag-", false);
        buffered += encodeMaskedFrame(WsOpcode::Continuation, "done", true);
        writeAll(ring, buffered);

        std::string message;
        WsOpcode opcode = WsOpcode::Close;
        galay::websocket::detail::WsMessageReadState state(
            ring, setting, message, opcode, true, false, nullptr);

        CHECK(expectSuccess(state, "fragmented message"), "fragmented message parse should succeed");
        CHECK(message == "frag-done", "fragmented message payload mismatch");
        CHECK(opcode == WsOpcode::Text, "fragmented message opcode mismatch");
        CHECK(ring.readable() == 0, "fragmented message should consume buffered frames");
        CHECK(state.m_fast_path_frames >= 2, "buffered fragmented message should batch-consume multiple frames");
    }

    {
        RingBuffer ring(1024);
        std::string buffered;
        buffered += encodeMaskedFrame(WsOpcode::Text, "part-", false);
        const auto trailing = encodeMaskedFrame(WsOpcode::Continuation, "tail", true);
        buffered += encodeMaskedFrame(WsOpcode::Ping, "mid-ping", true);
        buffered += trailing;
        writeAll(ring, buffered);

        std::vector<std::pair<WsOpcode, std::string>> controls;
        std::string message;
        WsOpcode opcode = WsOpcode::Close;
        galay::websocket::detail::WsMessageReadState state(
            ring,
            setting,
            message,
            opcode,
            true,
            false,
            [&](WsOpcode op, const std::string& payload) {
                controls.emplace_back(op, payload);
            });

        CHECK(expectSuccess(state, "control interleave"), "control interleave parse should succeed");
        CHECK(controls.size() == 1, "control interleave should surface one ping");
        CHECK(controls[0].first == WsOpcode::Ping, "control interleave callback opcode mismatch");
        CHECK(controls[0].second == "mid-ping", "control interleave callback payload mismatch");
        CHECK(message == "mid-ping", "control interleave surfaced message mismatch");
        CHECK(opcode == WsOpcode::Ping, "control interleave surfaced opcode mismatch");
        CHECK(ring.readable() == trailing.size(), "control interleave should leave trailing continuation buffered");
        CHECK(state.m_fast_path_frames >= 1, "reader should fast-path leading data frame before control fallback");
    }

    {
        RingBuffer ring(1024);
        const std::string invalid_utf8("\xFF\xFF", 2);
        const auto encoded = encodeMaskedFrame(WsOpcode::Text, invalid_utf8);
        writeAll(ring, encoded);

        std::string message;
        WsOpcode opcode = WsOpcode::Close;
        galay::websocket::detail::WsMessageReadState state(
            ring, setting, message, opcode, true, false, nullptr);

        CHECK(expectError(state, kWsInvalidUtf8, "invalid utf8"), "invalid utf8 should report parser error");
        CHECK(message.empty(), "invalid utf8 should not populate message");
        CHECK(opcode == WsOpcode::Close, "invalid utf8 should preserve initial opcode");
        CHECK(ring.readable() == encoded.size(), "invalid UTF-8 should preserve current non-consume error behavior");
    }

    std::cout << "T60-WsReaderFastPath PASS\n";
    return 0;
}
