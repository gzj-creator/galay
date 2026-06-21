/**
 * @file T31-H2FrameCodec.cc
 * @brief HTTP/2 frame codec contract test
 */

#include <galay/cpp/galay-http2/protoc/http2_frame.h>
#include <cassert>
#include <cstring>
#include <iostream>

using namespace galay::http2;

int main() {
    Http2PingFrame ping;
    uint8_t payload[8] = {1,2,3,4,5,6,7,8};
    ping.setOpaqueData(payload);
    ping.setAck(true);

    std::string bytes = Http2FrameCodec::encode(ping);
    auto parsed = Http2FrameCodec::decode(bytes);
    assert(parsed.has_value());
    assert(parsed.value()->isPing());

    auto* parsed_ping = parsed.value()->asPing();
    assert(parsed_ping != nullptr);
    assert(parsed_ping->isAck());
    assert(std::memcmp(parsed_ping->opaqueData(), payload, 8) == 0);

    std::cout << "T31-H2FrameCodec PASS\n";
    return 0;
}
