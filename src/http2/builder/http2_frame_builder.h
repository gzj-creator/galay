/**
 * @file http2_frame_builder.h
 * @brief HTTP/2 frame builder helper
 * @author galay-http
 * @version 1.0.0
 */

#ifndef GALAY_HTTP2_FRAME_BUILDER_H
#define GALAY_HTTP2_FRAME_BUILDER_H

#include "http2/protoc/http2_frame.h"

#include <array>
#include <memory>
#include <string>
#include <string_view>

namespace galay::http2
{

/**
 * @brief HTTP/2 帧构建器
 * @details 统一 DATA/HEADERS/RST_STREAM 的构建入口，减少散落的手工拼装逻辑。
 */
class Http2FrameBuilder
{
public:
    static std::unique_ptr<Http2DataFrame> data(uint32_t stream_id, std::string payload, bool end_stream = false);
    static std::unique_ptr<Http2HeadersFrame> headers(uint32_t stream_id,
                                                      std::string header_block,
                                                      bool end_stream = false,
                                                      bool end_headers = true);
    static std::unique_ptr<Http2RstStreamFrame> rstStream(uint32_t stream_id, Http2ErrorCode error);

    static std::array<char, kHttp2FrameHeaderLength> dataHeaderBytes(uint32_t stream_id,
                                                                     size_t payload_length,
                                                                     bool end_stream = false);
    static std::array<char, kHttp2FrameHeaderLength> headersHeaderBytes(uint32_t stream_id,
                                                                        size_t header_block_length,
                                                                        bool end_stream = false,
                                                                        bool end_headers = true);

    static std::string dataBytes(uint32_t stream_id, std::string_view payload, bool end_stream = false);
    static std::string headersBytes(uint32_t stream_id,
                                    std::string_view header_block,
                                    bool end_stream = false,
                                    bool end_headers = true);
    static std::string rstStreamBytes(uint32_t stream_id, Http2ErrorCode error);
};

} // namespace galay::http2

#endif // GALAY_HTTP2_FRAME_BUILDER_H
