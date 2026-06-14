/**
 * @file h2_helper.h
 * @brief HTTP/2 frame byte helper utilities
 * @author galay-http
 * @version 1.0.0
 */

#ifndef GALAY_H2_HELPER_H
#define GALAY_H2_HELPER_H

#include "http2/protoc/http2_base.h"

#include <array>
#include <string>
#include <string_view>

namespace galay::http2
{

/**
 * @brief Build the fixed 9-byte HTTP/2 frame header.
 * @param type HTTP/2 frame type.
 * @param flags HTTP/2 frame flags.
 * @param stream_id Stream identifier; the reserved high bit is cleared.
 * @param payload_length Frame payload length, encoded as a 24-bit big-endian value.
 * @return Serialized frame header bytes.
 */
std::array<char, kHttp2FrameHeaderLength> buildH2FrameHeaderBytes(Http2FrameType type,
                                                                  uint8_t flags,
                                                                  uint32_t stream_id,
                                                                  uint32_t payload_length);

/**
 * @brief Build a serialized HTTP/2 frame from header fields and payload bytes.
 * @param type HTTP/2 frame type.
 * @param flags HTTP/2 frame flags.
 * @param stream_id Stream identifier; the reserved high bit is cleared.
 * @param payload Frame payload view copied into the returned buffer.
 * @return Serialized frame bytes.
 */
std::string buildH2FrameBytes(Http2FrameType type,
                              uint8_t flags,
                              uint32_t stream_id,
                              std::string_view payload);

} // namespace galay::http2

#endif // GALAY_H2_HELPER_H
