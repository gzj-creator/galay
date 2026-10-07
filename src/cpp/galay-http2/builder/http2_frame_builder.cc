#include "http2_frame_builder.h"

#include "../utils/h2_helper.h"

#include <algorithm>
#include <utility>

namespace galay::http2
{

std::unique_ptr<Http2DataFrame> Http2FrameBuilder::data(uint32_t stream_id,
                                                        std::string payload,
                                                        bool end_stream)
{
    auto frame = std::make_unique<Http2DataFrame>();
    frame->header().stream_id = stream_id;
    frame->set_data(std::move(payload));
    frame->set_end_stream(end_stream);
    return frame;
}

std::unique_ptr<Http2HeadersFrame> Http2FrameBuilder::headers(uint32_t stream_id,
                                                              std::string header_block,
                                                              bool end_stream,
                                                              bool end_headers)
{
    auto frame = std::make_unique<Http2HeadersFrame>();
    frame->header().stream_id = stream_id;
    frame->set_header_block(std::move(header_block));
    frame->set_end_stream(end_stream);
    frame->set_end_headers(end_headers);
    return frame;
}

std::unique_ptr<Http2RstStreamFrame> Http2FrameBuilder::rst_stream(uint32_t stream_id, Http2ErrorCode error)
{
    auto frame = std::make_unique<Http2RstStreamFrame>();
    frame->header().stream_id = stream_id;
    frame->set_error_code(error);
    return frame;
}

std::array<char, kHttp2FrameHeaderLength> Http2FrameBuilder::data_header_bytes(uint32_t stream_id,
                                                                             size_t payload_length,
                                                                             bool end_stream)
{
    uint8_t flags = 0;
    if (end_stream) {
        flags |= Http2FrameFlags::kEndStream;
    }
    return build_h2_frame_header_bytes(
        Http2FrameType::Data, flags, stream_id, static_cast<uint32_t>(payload_length));
}

std::array<char, kHttp2FrameHeaderLength> Http2FrameBuilder::headers_header_bytes(uint32_t stream_id,
                                                                                size_t header_block_length,
                                                                                bool end_stream,
                                                                                bool end_headers)
{
    uint8_t flags = 0;
    if (end_stream) {
        flags |= Http2FrameFlags::kEndStream;
    }
    if (end_headers) {
        flags |= Http2FrameFlags::kEndHeaders;
    }
    return build_h2_frame_header_bytes(
        Http2FrameType::Headers, flags, stream_id, static_cast<uint32_t>(header_block_length));
}

std::array<char, kHttp2FrameHeaderLength> Http2FrameBuilder::continuation_header_bytes(
    uint32_t stream_id,
    size_t header_block_length,
    bool end_headers)
{
    uint8_t flags = 0;
    if (end_headers) {
        flags |= Http2FrameFlags::kEndHeaders;
    }
    return build_h2_frame_header_bytes(
        Http2FrameType::Continuation, flags, stream_id, static_cast<uint32_t>(header_block_length));
}

std::string Http2FrameBuilder::data_bytes(uint32_t stream_id,
                                         std::string_view payload,
                                         bool end_stream)
{
    if (payload.size() > kDefaultMaxFrameSize) {
        const size_t frame_count =
            (payload.size() + kDefaultMaxFrameSize - 1) / kDefaultMaxFrameSize;
        std::string result;
        result.reserve(payload.size() + frame_count * kHttp2FrameHeaderLength);

        size_t offset = 0;
        while (offset < payload.size()) {
            const size_t chunk_size = std::min<size_t>(
                payload.size() - offset, kDefaultMaxFrameSize);
            const bool chunk_end_stream = end_stream && offset + chunk_size == payload.size();
            result.append(data_bytes(
                stream_id, payload.substr(offset, chunk_size), chunk_end_stream));
            offset += chunk_size;
        }
        return result;
    }

    uint8_t flags = 0;
    if (end_stream) {
        flags |= Http2FrameFlags::kEndStream;
    }
    return build_h2_frame_bytes(Http2FrameType::Data, flags, stream_id, payload);
}

std::string Http2FrameBuilder::headers_bytes(uint32_t stream_id,
                                            std::string_view header_block,
                                            bool end_stream,
                                            bool end_headers)
{
    uint8_t flags = 0;
    if (end_stream) {
        flags |= Http2FrameFlags::kEndStream;
    }
    if (end_headers) {
        flags |= Http2FrameFlags::kEndHeaders;
    }
    return build_h2_frame_bytes(Http2FrameType::Headers, flags, stream_id, header_block);
}

std::string Http2FrameBuilder::continuation_bytes(uint32_t stream_id,
                                                 std::string_view header_block,
                                                 bool end_headers)
{
    uint8_t flags = 0;
    if (end_headers) {
        flags |= Http2FrameFlags::kEndHeaders;
    }
    return build_h2_frame_bytes(Http2FrameType::Continuation, flags, stream_id, header_block);
}

std::string Http2FrameBuilder::rst_stream_bytes(uint32_t stream_id, Http2ErrorCode error)
{
    char payload[4];
    const uint32_t code = static_cast<uint32_t>(error);
    payload[0] = static_cast<char>((code >> 24) & 0xFF);
    payload[1] = static_cast<char>((code >> 16) & 0xFF);
    payload[2] = static_cast<char>((code >> 8) & 0xFF);
    payload[3] = static_cast<char>(code & 0xFF);
    return build_h2_frame_bytes(Http2FrameType::RstStream, 0, stream_id,
                             std::string_view(payload, sizeof(payload)));
}

} // namespace galay::http2
