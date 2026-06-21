/**
 * @file T37-H2FrameBuilderbytes.cc
 * @brief HTTP/2 buffered batch-frame parsing contract
 */

#include <galay/cpp/galay-http2/kernel/http2_conn.h>
#include <galay/cpp/galay-http2/protoc/http2_frame.h>
#include <galay/cpp/galay-http2/protoc/http2_base.h>
#include <expected>
#include <iostream>
#include <cassert>
#include <cstring>
#include <vector>

using namespace galay::http2;
using namespace galay::kernel;
using namespace galay::async;

using BatchParseResult = std::expected<std::vector<Http2Frame::uptr>, Http2ErrorCode>;

std::string flattenHeaderAndPayload(const std::array<char, kHttp2FrameHeaderLength>& header,
                                    std::string_view payload) {
    std::string bytes;
    bytes.reserve(header.size() + payload.size());
    bytes.append(header.data(), header.size());
    bytes.append(payload.data(), payload.size());
    return bytes;
}

// Helper: serialize a PING frame (8 bytes payload)
std::vector<uint8_t> buildPingFrame(uint64_t opaque_data) {
    std::vector<uint8_t> frame(9 + 8);
    // Header: length=8, type=PING(6), flags=0, stream_id=0
    frame[0] = 0; frame[1] = 0; frame[2] = 8;  // length
    frame[3] = 6;  // type PING
    frame[4] = 0;  // flags
    frame[5] = frame[6] = frame[7] = frame[8] = 0;  // stream_id=0
    // Payload: 8 bytes opaque data
    for (int i = 0; i < 8; ++i) {
        frame[9 + i] = static_cast<uint8_t>((opaque_data >> (56 - i * 8)) & 0xFF);
    }
    return frame;
}

// Helper: serialize a SETTINGS frame (empty)
std::vector<uint8_t> buildSettingsFrame() {
    std::vector<uint8_t> frame(9);
    // Header: length=0, type=SETTINGS(4), flags=0, stream_id=0
    frame[0] = frame[1] = frame[2] = 0;  // length=0
    frame[3] = 4;  // type SETTINGS
    frame[4] = 0;  // flags
    frame[5] = frame[6] = frame[7] = frame[8] = 0;  // stream_id=0
    return frame;
}

// Helper: serialize a frame with given length (for oversize test)
std::vector<uint8_t> buildOversizeFrame(uint32_t payload_len) {
    std::vector<uint8_t> frame(9 + payload_len);
    // Header: length=payload_len, type=DATA(0), flags=0, stream_id=1
    frame[0] = static_cast<uint8_t>((payload_len >> 16) & 0xFF);
    frame[1] = static_cast<uint8_t>((payload_len >> 8) & 0xFF);
    frame[2] = static_cast<uint8_t>(payload_len & 0xFF);
    frame[3] = 0;  // type DATA
    frame[4] = 0;  // flags
    frame[5] = frame[6] = frame[7] = 0;
    frame[8] = 1;  // stream_id=1
    // Fill payload with dummy data
    for (uint32_t i = 0; i < payload_len; ++i) {
        frame[9 + i] = static_cast<uint8_t>(i & 0xFF);
    }
    return frame;
}

int main() {
    std::cout << "[T37] Starting HTTP/2 buffered batch-frame parsing tests\n";

    // ========== Static contract checks ==========
    static_assert(requires(Http2Conn* conn, const char* data, size_t len) {
        conn->feedData(data, len);
    }, "Http2Conn must keep feedData(data, len) for byte-oriented tests");

    static_assert(requires(Http2Conn* conn) {
        conn->peerSettings();
    }, "Http2Conn must expose peerSettings() for max-frame-size validation");

    static_assert(requires(Http2Conn* conn) {
        { conn->parseBufferedFrames(16) } -> std::same_as<BatchParseResult>;
    }, "Http2Conn must expose parseBufferedFrames(max_count) -> expected<vector<frame>, error>");

    std::cout << "[T37] Static contract checks PASS\n";

    // ========== Scenario 1: Multiple complete buffered frames ==========
    {
        std::cout << "[T37] Scenario 1: Multiple complete buffered frames\n";

        TcpSocket socket(GHandle{-1});
        Http2Conn conn(std::move(socket));

        // Build 3 PING frames
        auto ping1 = buildPingFrame(0x1111111111111111ULL);
        auto ping2 = buildPingFrame(0x2222222222222222ULL);
        auto ping3 = buildPingFrame(0x3333333333333333ULL);

        // Feed all 3 frames into buffer
        conn.feedData(reinterpret_cast<const char*>(ping1.data()), ping1.size());
        conn.feedData(reinterpret_cast<const char*>(ping2.data()), ping2.size());
        conn.feedData(reinterpret_cast<const char*>(ping3.data()), ping3.size());

        // Parse with max_count=10 (should get all 3)
        auto result = conn.parseBufferedFrames(10);
        assert(result.has_value() && "Should parse successfully");
        assert(result->size() == 3 && "Should return 3 frames");

        // Verify frame types
        assert(result->at(0)->header().type == Http2FrameType::Ping);
        assert(result->at(1)->header().type == Http2FrameType::Ping);
        assert(result->at(2)->header().type == Http2FrameType::Ping);

        std::cout << "[T37] Scenario 1 PASS: parsed 3 complete frames\n";
    }

    // ========== Scenario 2: Complete + partial trailing frame ==========
    {
        std::cout << "[T37] Scenario 2: Complete frame + partial trailing frame\n";

        TcpSocket socket(GHandle{-1});
        Http2Conn conn(std::move(socket));

        // Build 2 frames
        auto settings = buildSettingsFrame();
        auto ping = buildPingFrame(0x4444444444444444ULL);

        // Feed complete SETTINGS + partial PING (only 5 bytes of header)
        conn.feedData(reinterpret_cast<const char*>(settings.data()), settings.size());
        conn.feedData(reinterpret_cast<const char*>(ping.data()), 5);

        // Parse should return only the complete SETTINGS frame
        auto result = conn.parseBufferedFrames(10);
        assert(result.has_value() && "Should parse successfully");
        assert(result->size() == 1 && "Should return only 1 complete frame");
        assert(result->at(0)->header().type == Http2FrameType::Settings);

        // Verify partial frame is still buffered (5 bytes remain)
        assert(conn.ringBuffer().readable() == 5 && "Partial frame should remain buffered");

        std::cout << "[T37] Scenario 2 PASS: returned 1 complete, left partial buffered\n";
    }

    // ========== Scenario 3: Oversize frame header validation ==========
    {
        std::cout << "[T37] Scenario 3: Oversize frame header validation\n";

        TcpSocket socket(GHandle{-1});
        Http2Conn conn(std::move(socket));

        // Get peer max_frame_size (default is 16384)
        uint32_t max_size = conn.peerSettings().max_frame_size;
        assert(max_size == kDefaultMaxFrameSize && "Default max_frame_size should be 16384");

        // Build a frame with length = max_size + 1 (oversize)
        auto oversize = buildOversizeFrame(max_size + 1);

        // Feed only the header (9 bytes) - enough to detect oversize
        conn.feedData(reinterpret_cast<const char*>(oversize.data()), 9);

        // Parse should return FrameSizeError
        auto result = conn.parseBufferedFrames(10);
        assert(!result.has_value() && "Should fail on oversize frame");
        assert(result.error() == Http2ErrorCode::FrameSizeError && "Should return FrameSizeError");

        std::cout << "[T37] Scenario 3 PASS: detected FrameSizeError for oversize frame\n";
    }

    // ========== Scenario 4: max_count limit ==========
    {
        std::cout << "[T37] Scenario 4: max_count limit\n";

        TcpSocket socket(GHandle{-1});
        Http2Conn conn(std::move(socket));

        // Build 5 SETTINGS frames
        for (int i = 0; i < 5; ++i) {
            auto settings = buildSettingsFrame();
            conn.feedData(reinterpret_cast<const char*>(settings.data()), settings.size());
        }

        // Parse with max_count=3 (should get only 3)
        auto result = conn.parseBufferedFrames(3);
        assert(result.has_value() && "Should parse successfully");
        assert(result->size() == 3 && "Should return exactly 3 frames");

        // Verify 2 frames remain buffered (2 * 9 = 18 bytes)
        assert(conn.ringBuffer().readable() == 18 && "2 frames should remain buffered");

        std::cout << "[T37] Scenario 4 PASS: respected max_count limit\n";
    }

    // ========== Scenario 5: Empty buffer ==========
    {
        std::cout << "[T37] Scenario 5: Empty buffer\n";

        TcpSocket socket(GHandle{-1});
        Http2Conn conn(std::move(socket));

        // Parse empty buffer
        auto result = conn.parseBufferedFrames(10);
        assert(result.has_value() && "Should succeed on empty buffer");
        assert(result->empty() && "Should return empty vector");

        std::cout << "[T37] Scenario 5 PASS: empty buffer returns empty vector\n";
    }

    std::cout << "[T37] All scenarios PASS\n";

    // ========== Scenario 5.5: segmented builder bytes equivalence ==========
    {
        std::cout << "[T37] Scenario 5.5: segmented builder bytes equivalence\n";

        static_assert(requires {
            { Http2FrameBuilder::dataHeaderBytes(7, 5, true) }
                -> std::same_as<std::array<char, kHttp2FrameHeaderLength>>;
        }, "Http2FrameBuilder must expose dataHeaderBytes(stream_id, payload_len, end_stream)");

        static_assert(requires {
            { Http2FrameBuilder::headersHeaderBytes(3, 11, false, true) }
                -> std::same_as<std::array<char, kHttp2FrameHeaderLength>>;
        }, "Http2FrameBuilder must expose headersHeaderBytes(stream_id, header_block_len, end_stream, end_headers)");

        const std::string payload = "hello";
        const auto data_header = Http2FrameBuilder::dataHeaderBytes(7, payload.size(), true);
        const auto data_bytes = Http2FrameBuilder::dataBytes(7, payload, true);
        assert(flattenHeaderAndPayload(data_header, payload) == data_bytes &&
               "Segmented DATA header bytes must match legacy full-frame bytes");

        const std::string header_block = "header-block";
        const auto headers_header =
            Http2FrameBuilder::headersHeaderBytes(3, header_block.size(), false, true);
        const auto headers_bytes =
            Http2FrameBuilder::headersBytes(3, header_block, false, true);
        assert(flattenHeaderAndPayload(headers_header, header_block) == headers_bytes &&
               "Segmented HEADERS header bytes must match legacy full-frame bytes");

        std::cout << "[T37] Scenario 5.5 PASS: segmented header bytes match legacy builders\n";
    }

    // ========== NEW: Batch awaitable contract tests ==========
    std::cout << "\n[T37] Starting batch awaitable contract tests\n";

    // ========== Scenario 6: readFramesBatch() static contract ==========
    {
        std::cout << "[T37] Scenario 6: readFramesBatch() static contract\n";

        static_assert(requires(Http2Conn* conn) {
            { conn->readFramesBatch() };
        }, "Http2Conn must expose readFramesBatch() method");

        static_assert(requires(Http2Conn* conn) {
            { conn->readFramesBatch(10) };
        }, "Http2Conn must accept max_frames parameter");

        std::cout << "[T37] Scenario 6 PASS: readFramesBatch() contract exists\n";
    }

    // ========== Scenario 7: Batch awaitable with buffered frames ==========
    {
        std::cout << "[T37] Scenario 7: Batch awaitable with buffered frames (await_ready)\n";

        TcpSocket socket(GHandle{-1});
        Http2Conn conn(std::move(socket));

        // Build and feed 3 PING frames
        auto ping1 = buildPingFrame(0x1111111111111111ULL);
        auto ping2 = buildPingFrame(0x2222222222222222ULL);
        auto ping3 = buildPingFrame(0x3333333333333333ULL);

        conn.feedData(reinterpret_cast<const char*>(ping1.data()), ping1.size());
        conn.feedData(reinterpret_cast<const char*>(ping2.data()), ping2.size());
        conn.feedData(reinterpret_cast<const char*>(ping3.data()), ping3.size());

        // Create awaitable - should be ready immediately
        auto awaitable = conn.readFramesBatch(10);
        assert(awaitable.await_ready() && "Should be ready when frames buffered");

        // Note: We can't test await_resume() without coroutine context,
        // but await_ready() validates the core contract

        std::cout << "[T37] Scenario 7 PASS: await_ready() returns true with buffered frames\n";
    }

    // ========== Scenario 8: Batch awaitable with empty buffer ==========
    {
        std::cout << "[T37] Scenario 8: Batch awaitable with empty buffer (suspends)\n";

        TcpSocket socket(GHandle{-1});
        Http2Conn conn(std::move(socket));

        // Empty buffer - awaitable should NOT be ready (will suspend)
        auto awaitable = conn.readFramesBatch(10);
        assert(!awaitable.await_ready() && "Should NOT be ready when buffer empty");

        std::cout << "[T37] Scenario 8 PASS: await_ready() returns false with empty buffer\n";
    }

    // ========== Scenario 9: Batch awaitable respects max_frames limit ==========
    {
        std::cout << "[T37] Scenario 9: Batch awaitable respects max_frames in await_ready\n";

        TcpSocket socket(GHandle{-1});
        Http2Conn conn(std::move(socket));

        // Build 5 frames
        for (int i = 0; i < 5; ++i) {
            auto settings = buildSettingsFrame();
            conn.feedData(reinterpret_cast<const char*>(settings.data()), settings.size());
        }

        // Create awaitable with max_frames=3
        auto awaitable = conn.readFramesBatch(3);
        assert(awaitable.await_ready() && "Should be ready when frames available");

        std::cout << "[T37] Scenario 9 PASS: await_ready() works with max_frames limit\n";
    }

    std::cout << "[T37] All batch awaitable contract tests PASS\n";
    return 0;
}
