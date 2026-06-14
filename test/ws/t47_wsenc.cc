#include "ws/protoc/ws_frame.h"
#include <array>
#include <cassert>
#include <cstring>
#include <iostream>

using namespace galay::websocket;

int main() {
    static_assert(requires(std::string& out, const WsFrame& frame) {
        WsFrameParser::encodeInto(out, frame, false);
    }, "WsFrameParser must expose encodeInto(out, frame, use_mask)");

    {
        WsFrame frame = WsFrameParser::createTextFrame("hello websocket");
        std::string expected = WsFrameParser::toBytes(frame, false);

        std::string reused = "stale-prefix";
        reused.reserve(256);
        const auto* original_ptr = reused.data();

        WsFrameParser::encodeInto(reused, frame, false);

        assert(reused == expected);
        assert(reused.data() == original_ptr && "encodeInto should reuse caller-owned capacity when possible");
    }

    {
        std::string payload(1024, 'A');
        WsFrame frame = WsFrameParser::createBinaryFrame(payload);
        std::string expected = WsFrameParser::toBytes(frame, false);

        std::string reused(8, 'x');
        reused.reserve(expected.size() + 64);
        const auto* original_ptr = reused.data();

        WsFrameParser::encodeInto(reused, frame, false);
        assert(reused == expected);
        assert(reused.data() == original_ptr && "encodeInto should avoid reallocating when capacity is already sufficient");

        reused = "trailing-garbage-that-must-disappear";
        reused.reserve(expected.size() + 64);
        WsFrameParser::encodeInto(reused, frame, false);
        assert(reused == expected && "encodeInto must overwrite previous contents, not append to them");
    }

    {
        std::string payload(300, 'Z');
        WsFrame frame = WsFrameParser::createTextFrame(payload);

        std::string encoded;
        WsFrameParser::encodeInto(encoded, frame, true);

        std::array<iovec, 1> iovecs{{
            {
                .iov_base = encoded.data(),
                .iov_len = encoded.size(),
            }
        }};

        WsFrame parsed;
        auto parsed_result = WsFrameParser::fromIOVec(iovecs.data(), iovecs.size(), parsed, true);
        assert(parsed_result.has_value());
        assert(parsed.payload == payload);
        assert(parsed.header.opcode == WsOpcode::Text);
    }

    std::cout << "T47-WsEncodeInto PASS\n";
    return 0;
}
