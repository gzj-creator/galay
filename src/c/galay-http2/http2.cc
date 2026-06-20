#include "http2.h"

#include <galay/cpp/galay-http2/protoc/http2_base.h>
#include <galay/cpp/galay-http2/protoc/http2_frame.h>
#include <galay/cpp/galay-http2/protoc/http2_hpack.h>

#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <vector>

struct galay_http2_frame {
    std::unique_ptr<galay::http2::Http2Frame> frame;
};

struct galay_http2_headers {
    std::vector<galay::http2::Http2HeaderField> fields;
};

namespace {

galay_status_t map_http2_error(galay::http2::Http2ErrorCode code)
{
    return code == galay::http2::Http2ErrorCode::NoError ? GALAY_OK : GALAY_PROTOCOL_ERROR;
}

galay_http2_frame_type_t map_frame_type(galay::http2::Http2FrameType type)
{
    return static_cast<galay_http2_frame_type_t>(static_cast<uint8_t>(type));
}

bool frame_length_matches(const uint8_t* bytes, size_t length)
{
    if (length < galay::http2::kHttp2FrameHeaderLength) {
        return false;
    }
    const uint32_t payload_length = (static_cast<uint32_t>(bytes[0]) << 16) |
                                    (static_cast<uint32_t>(bytes[1]) << 8) |
                                    static_cast<uint32_t>(bytes[2]);
    return payload_length <= galay::http2::kMaxFrameSize &&
           length == galay::http2::kHttp2FrameHeaderLength + payload_length;
}

} // namespace

extern "C" {

galay_status_t galay_http2_ping_frame_create(const uint8_t* opaque_data,
                                             galay_bool_t ack,
                                             galay_http2_frame_t** out_frame)
{
    if (opaque_data == nullptr || out_frame == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    *out_frame = nullptr;
    try {
        auto ping = std::make_unique<galay::http2::Http2PingFrame>();
        ping->setOpaqueData(opaque_data);
        ping->setAck(ack != GALAY_FALSE);

        auto handle = std::make_unique<galay_http2_frame>();
        handle->frame = std::move(ping);
        *out_frame = handle.release();
        return GALAY_OK;
    } catch (const std::bad_alloc&) {
        return GALAY_OUT_OF_MEMORY;
    } catch (...) {
        return GALAY_INTERNAL_ERROR;
    }
}

void galay_http2_frame_destroy(galay_http2_frame_t* frame)
{
    delete frame;
}

galay_status_t galay_http2_frame_encode(const galay_http2_frame_t* frame,
                                        uint8_t* out_bytes,
                                        size_t* inout_length)
{
    if (frame == nullptr || frame->frame == nullptr || inout_length == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    try {
        const std::string encoded = galay::http2::Http2FrameCodec::encode(*frame->frame);
        const size_t required = encoded.size();
        if (out_bytes == nullptr || *inout_length < required) {
            *inout_length = required;
            return GALAY_INVALID_ARGUMENT;
        }
        std::memcpy(out_bytes, encoded.data(), required);
        *inout_length = required;
        return GALAY_OK;
    } catch (const std::bad_alloc&) {
        return GALAY_OUT_OF_MEMORY;
    } catch (...) {
        return GALAY_INTERNAL_ERROR;
    }
}

galay_status_t galay_http2_frame_decode(const uint8_t* bytes,
                                        size_t length,
                                        galay_http2_frame_t** out_frame)
{
    if (out_frame == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    *out_frame = nullptr;
    if (bytes == nullptr || !frame_length_matches(bytes, length)) {
        return length < galay::http2::kHttp2FrameHeaderLength ? GALAY_INVALID_ARGUMENT
                                                              : GALAY_PROTOCOL_ERROR;
    }
    try {
        const std::string_view view(reinterpret_cast<const char*>(bytes), length);
        auto decoded = galay::http2::Http2FrameCodec::decode(view);
        if (!decoded) {
            return map_http2_error(decoded.error());
        }

        auto handle = std::make_unique<galay_http2_frame>();
        handle->frame = std::move(*decoded);
        *out_frame = handle.release();
        return GALAY_OK;
    } catch (const std::bad_alloc&) {
        return GALAY_OUT_OF_MEMORY;
    } catch (...) {
        return GALAY_INTERNAL_ERROR;
    }
}

galay_http2_frame_type_t galay_http2_frame_type(const galay_http2_frame_t* frame)
{
    if (frame == nullptr || frame->frame == nullptr) {
        return GALAY_HTTP2_FRAME_UNKNOWN;
    }
    return map_frame_type(frame->frame->type());
}

uint32_t galay_http2_frame_stream_id(const galay_http2_frame_t* frame)
{
    if (frame == nullptr || frame->frame == nullptr) {
        return 0;
    }
    return frame->frame->streamId();
}

galay_status_t galay_http2_ping_frame_opaque(const galay_http2_frame_t* frame,
                                             uint8_t out_opaque_data[8])
{
    if (frame == nullptr || frame->frame == nullptr || out_opaque_data == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    const auto* ping = frame->frame->asPing();
    if (ping == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    std::memcpy(out_opaque_data, ping->opaqueData(), 8);
    return GALAY_OK;
}

galay_status_t galay_http2_headers_create(galay_http2_headers_t** out_headers)
{
    if (out_headers == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    *out_headers = nullptr;
    try {
        *out_headers = new galay_http2_headers();
        return GALAY_OK;
    } catch (const std::bad_alloc&) {
        return GALAY_OUT_OF_MEMORY;
    } catch (...) {
        return GALAY_INTERNAL_ERROR;
    }
}

void galay_http2_headers_destroy(galay_http2_headers_t* headers)
{
    delete headers;
}

galay_status_t galay_http2_headers_add(galay_http2_headers_t* headers,
                                       const char* name,
                                       const char* value)
{
    if (headers == nullptr || name == nullptr || value == nullptr || name[0] == '\0') {
        return GALAY_INVALID_ARGUMENT;
    }
    try {
        headers->fields.push_back({name, value});
        return GALAY_OK;
    } catch (const std::bad_alloc&) {
        return GALAY_OUT_OF_MEMORY;
    } catch (...) {
        return GALAY_INTERNAL_ERROR;
    }
}

size_t galay_http2_headers_count(const galay_http2_headers_t* headers)
{
    return headers == nullptr ? 0 : headers->fields.size();
}

galay_status_t galay_http2_headers_get(const galay_http2_headers_t* headers,
                                       size_t index,
                                       const char** out_name,
                                       const char** out_value)
{
    if (headers == nullptr || out_name == nullptr || out_value == nullptr ||
        index >= headers->fields.size()) {
        return GALAY_INVALID_ARGUMENT;
    }
    *out_name = headers->fields[index].name.c_str();
    *out_value = headers->fields[index].value.c_str();
    return GALAY_OK;
}

galay_status_t galay_http2_hpack_encode(const galay_http2_headers_t* headers,
                                        uint8_t* out_bytes,
                                        size_t* inout_length)
{
    if (headers == nullptr || inout_length == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    try {
        galay::http2::HpackEncoder encoder;
        const std::string block = encoder.encodeStateless(headers->fields);
        const size_t required = block.size();
        if (out_bytes == nullptr || *inout_length < required) {
            *inout_length = required;
            return GALAY_INVALID_ARGUMENT;
        }
        std::memcpy(out_bytes, block.data(), required);
        *inout_length = required;
        return GALAY_OK;
    } catch (const std::bad_alloc&) {
        return GALAY_OUT_OF_MEMORY;
    } catch (...) {
        return GALAY_INTERNAL_ERROR;
    }
}

galay_status_t galay_http2_hpack_decode(const uint8_t* bytes,
                                        size_t length,
                                        galay_http2_headers_t** out_headers)
{
    if (out_headers == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    *out_headers = nullptr;
    if (bytes == nullptr && length != 0) {
        return GALAY_INVALID_ARGUMENT;
    }
    try {
        galay::http2::HpackDecoder decoder;
        auto decoded = decoder.decode(bytes, length);
        if (!decoded) {
            return map_http2_error(decoded.error());
        }
        auto headers = std::make_unique<galay_http2_headers>();
        headers->fields = std::move(*decoded);
        *out_headers = headers.release();
        return GALAY_OK;
    } catch (const std::bad_alloc&) {
        return GALAY_OUT_OF_MEMORY;
    } catch (...) {
        return GALAY_INTERNAL_ERROR;
    }
}

galay_status_t galay_http2_stream_id_validate(uint32_t stream_id, galay_bool_t allow_zero)
{
    if ((stream_id & 0x80000000u) != 0) {
        return GALAY_INVALID_ARGUMENT;
    }
    if (stream_id == 0 && allow_zero == GALAY_FALSE) {
        return GALAY_INVALID_ARGUMENT;
    }
    return GALAY_OK;
}

galay_status_t galay_http2_settings_value_validate(galay_http2_settings_id_t id, uint32_t value)
{
    switch (id) {
        case GALAY_HTTP2_SETTINGS_HEADER_TABLE_SIZE:
        case GALAY_HTTP2_SETTINGS_MAX_CONCURRENT_STREAMS:
        case GALAY_HTTP2_SETTINGS_MAX_HEADER_LIST_SIZE:
            return GALAY_OK;
        case GALAY_HTTP2_SETTINGS_ENABLE_PUSH:
            return value <= 1u ? GALAY_OK : GALAY_PROTOCOL_ERROR;
        case GALAY_HTTP2_SETTINGS_INITIAL_WINDOW_SIZE:
            return value <= 2147483647u ? GALAY_OK : GALAY_PROTOCOL_ERROR;
        case GALAY_HTTP2_SETTINGS_MAX_FRAME_SIZE:
            return value >= 16384u && value <= 16777215u ? GALAY_OK : GALAY_PROTOCOL_ERROR;
        default:
            return GALAY_INVALID_ARGUMENT;
    }
}

} // extern "C"
