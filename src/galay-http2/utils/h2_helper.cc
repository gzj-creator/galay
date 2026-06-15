#include "h2_helper.h"

#include <cstring>

namespace galay::http2
{

std::array<char, kHttp2FrameHeaderLength> buildH2FrameHeaderBytes(Http2FrameType type,
                                                                  uint8_t flags,
                                                                  uint32_t stream_id,
                                                                  uint32_t payload_length)
{
    std::array<char, kHttp2FrameHeaderLength> bytes{};
    bytes[0] = static_cast<char>((payload_length >> 16) & 0xFF);
    bytes[1] = static_cast<char>((payload_length >> 8) & 0xFF);
    bytes[2] = static_cast<char>(payload_length & 0xFF);
    bytes[3] = static_cast<char>(type);
    bytes[4] = static_cast<char>(flags);

    const uint32_t sid = stream_id & kMaxStreamId;
    bytes[5] = static_cast<char>((sid >> 24) & 0xFF);
    bytes[6] = static_cast<char>((sid >> 16) & 0xFF);
    bytes[7] = static_cast<char>((sid >> 8) & 0xFF);
    bytes[8] = static_cast<char>(sid & 0xFF);
    return bytes;
}

std::string buildH2FrameBytes(Http2FrameType type,
                              uint8_t flags,
                              uint32_t stream_id,
                              std::string_view payload)
{
    std::string result;
    result.resize(kHttp2FrameHeaderLength + payload.size());

    const auto header = buildH2FrameHeaderBytes(
        type, flags, stream_id, static_cast<uint32_t>(payload.size()));
    std::memcpy(result.data(), header.data(), header.size());

    if (!payload.empty()) {
        std::memcpy(result.data() + kHttp2FrameHeaderLength, payload.data(), payload.size());
    }
    return result;
}

} // namespace galay::http2
