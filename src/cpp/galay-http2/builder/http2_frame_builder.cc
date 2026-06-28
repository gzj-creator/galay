#include <galay/cpp/galay-http2/builder/http2_frame_builder.h>

#include <galay/cpp/galay-http2/utils/h2_helper.h>

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
    frame->setData(std::move(payload));
    frame->setEndStream(end_stream);
    return frame;
}

std::unique_ptr<Http2HeadersFrame> Http2FrameBuilder::headers(uint32_t stream_id,
                                                              std::string header_block,
                                                              bool end_stream,
                                                              bool end_headers)
{
    auto frame = std::make_unique<Http2HeadersFrame>();
    frame->header().stream_id = stream_id;
    frame->setHeaderBlock(std::move(header_block));
    frame->setEndStream(end_stream);
    frame->setEndHeaders(end_headers);
    return frame;
}

std::unique_ptr<Http2RstStreamFrame> Http2FrameBuilder::rstStream(uint32_t stream_id, Http2ErrorCode error)
{
    auto frame = std::make_unique<Http2RstStreamFrame>();
    frame->header().stream_id = stream_id;
    frame->setErrorCode(error);
    return frame;
}

std::array<char, kHttp2FrameHeaderLength> Http2FrameBuilder::dataHeaderBytes(uint32_t stream_id,
                                                                             size_t payload_length,
                                                                             bool end_stream)
{
    uint8_t flags = 0;
    if (end_stream) {
        flags |= Http2FrameFlags::kEndStream;
    }
    return buildH2FrameHeaderBytes(
        Http2FrameType::Data, flags, stream_id, static_cast<uint32_t>(payload_length));
}

std::array<char, kHttp2FrameHeaderLength> Http2FrameBuilder::headersHeaderBytes(uint32_t stream_id,
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
    return buildH2FrameHeaderBytes(
        Http2FrameType::Headers, flags, stream_id, static_cast<uint32_t>(header_block_length));
}

std::array<char, kHttp2FrameHeaderLength> Http2FrameBuilder::continuationHeaderBytes(
    uint32_t stream_id,
    size_t header_block_length,
    bool end_headers)
{
    uint8_t flags = 0;
    if (end_headers) {
        flags |= Http2FrameFlags::kEndHeaders;
    }
    return buildH2FrameHeaderBytes(
        Http2FrameType::Continuation, flags, stream_id, static_cast<uint32_t>(header_block_length));
}

std::string Http2FrameBuilder::dataBytes(uint32_t stream_id,
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
            result.append(dataBytes(
                stream_id, payload.substr(offset, chunk_size), chunk_end_stream));
            offset += chunk_size;
        }
        return result;
    }

    uint8_t flags = 0;
    if (end_stream) {
        flags |= Http2FrameFlags::kEndStream;
    }
    return buildH2FrameBytes(Http2FrameType::Data, flags, stream_id, payload);
}

std::string Http2FrameBuilder::headersBytes(uint32_t stream_id,
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
    return buildH2FrameBytes(Http2FrameType::Headers, flags, stream_id, header_block);
}

std::string Http2FrameBuilder::continuationBytes(uint32_t stream_id,
                                                 std::string_view header_block,
                                                 bool end_headers)
{
    uint8_t flags = 0;
    if (end_headers) {
        flags |= Http2FrameFlags::kEndHeaders;
    }
    return buildH2FrameBytes(Http2FrameType::Continuation, flags, stream_id, header_block);
}

std::string Http2FrameBuilder::rstStreamBytes(uint32_t stream_id, Http2ErrorCode error)
{
    char payload[4];
    const uint32_t code = static_cast<uint32_t>(error);
    payload[0] = static_cast<char>((code >> 24) & 0xFF);
    payload[1] = static_cast<char>((code >> 16) & 0xFF);
    payload[2] = static_cast<char>((code >> 8) & 0xFF);
    payload[3] = static_cast<char>(code & 0xFF);
    return buildH2FrameBytes(Http2FrameType::RstStream, 0, stream_id,
                             std::string_view(payload, sizeof(payload)));
}

} // namespace galay::http2
