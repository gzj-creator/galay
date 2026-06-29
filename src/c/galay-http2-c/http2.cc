#include <galay/c/galay-http2-c/http2.h>

#include <cstring>
#include <new>
#include <string>
#include <vector>

struct galay_http2_frame_t {
    galay_http2_frame_type_t type = GALAY_HTTP2_FRAME_DATA;
    uint8_t flags = 0;
    uint32_t stream_id = 0;
    std::vector<uint8_t> payload;
};

struct galay_http2_header_item {
    std::string name;
    std::string value;
};

struct galay_http2_headers_t {
    std::vector<galay_http2_header_item> entries;
};

extern "C" {

const char* galay_http2_get_error(galay_status_t status)
{
    return galay_status_string(status);
}

galay_status_t galay_http2_stream_id_validate(uint32_t stream_id, galay_bool_t allow_zero)
{
    if ((stream_id & 0x80000000u) != 0) {
        return GALAY_INVALID_ARGUMENT;
    }
    if (stream_id == 0 && allow_zero != GALAY_TRUE) {
        return GALAY_INVALID_ARGUMENT;
    }
    return GALAY_OK;
}

galay_status_t galay_http2_settings_value_validate(galay_http2_settings_id_t id, uint32_t value)
{
    if (id == GALAY_HTTP2_SETTINGS_ENABLE_PUSH && value > 1) {
        return GALAY_PROTOCOL_ERROR;
    }
    if (id == GALAY_HTTP2_SETTINGS_INITIAL_WINDOW_SIZE && value > 2147483647u) {
        return GALAY_PROTOCOL_ERROR;
    }
    if (id == GALAY_HTTP2_SETTINGS_MAX_FRAME_SIZE && (value < 16384u || value > 16777215u)) {
        return GALAY_PROTOCOL_ERROR;
    }
    return GALAY_OK;
}

galay_status_t galay_http2_ping_frame_create(const uint8_t opaque[8], galay_bool_t ack,
                                             galay_http2_frame_t** out)
{
    if (opaque == nullptr || out == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    auto* frame = new (std::nothrow) galay_http2_frame_t();
    if (frame == nullptr) {
        *out = nullptr;
        return GALAY_OUT_OF_MEMORY;
    }
    frame->type = GALAY_HTTP2_FRAME_PING;
    frame->flags = ack == GALAY_TRUE ? 0x1 : 0x0;
    frame->payload.assign(opaque, opaque + 8);
    *out = frame;
    return GALAY_OK;
}

void galay_http2_frame_destroy(galay_http2_frame_t* frame)
{
    delete frame;
}

galay_status_t galay_http2_frame_encode(const galay_http2_frame_t* frame, uint8_t* out,
                                        size_t* out_len)
{
    if (frame == nullptr || out_len == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    const size_t need = GALAY_HTTP2_FRAME_HEADER_LENGTH + frame->payload.size();
    if (out == nullptr || *out_len < need) {
        *out_len = need;
        return GALAY_OUT_OF_MEMORY;
    }
    const uint32_t len = static_cast<uint32_t>(frame->payload.size());
    out[0] = static_cast<uint8_t>((len >> 16U) & 0xFFU);
    out[1] = static_cast<uint8_t>((len >> 8U) & 0xFFU);
    out[2] = static_cast<uint8_t>(len & 0xFFU);
    out[3] = static_cast<uint8_t>(frame->type);
    out[4] = frame->flags;
    out[5] = static_cast<uint8_t>((frame->stream_id >> 24U) & 0x7FU);
    out[6] = static_cast<uint8_t>((frame->stream_id >> 16U) & 0xFFU);
    out[7] = static_cast<uint8_t>((frame->stream_id >> 8U) & 0xFFU);
    out[8] = static_cast<uint8_t>(frame->stream_id & 0xFFU);
    if (!frame->payload.empty()) {
        std::memcpy(out + GALAY_HTTP2_FRAME_HEADER_LENGTH, frame->payload.data(), frame->payload.size());
    }
    *out_len = need;
    return GALAY_OK;
}

galay_status_t galay_http2_frame_decode(const uint8_t* data, size_t data_len,
                                        galay_http2_frame_t** out)
{
    if (out != nullptr) {
        *out = nullptr;
    }
    if (data == nullptr || out == nullptr || data_len < GALAY_HTTP2_FRAME_HEADER_LENGTH) {
        return GALAY_INVALID_ARGUMENT;
    }
    const uint32_t len = (static_cast<uint32_t>(data[0]) << 16U) |
        (static_cast<uint32_t>(data[1]) << 8U) | data[2];
    if (data_len < GALAY_HTTP2_FRAME_HEADER_LENGTH + len) {
        return GALAY_PROTOCOL_ERROR;
    }
    const auto type = static_cast<galay_http2_frame_type_t>(data[3]);
    const uint32_t stream_id = (static_cast<uint32_t>(data[5] & 0x7FU) << 24U) |
        (static_cast<uint32_t>(data[6]) << 16U) |
        (static_cast<uint32_t>(data[7]) << 8U) | data[8];
    if (type == GALAY_HTTP2_FRAME_PING && (len != 8 || stream_id != 0)) {
        return GALAY_PROTOCOL_ERROR;
    }
    auto* frame = new (std::nothrow) galay_http2_frame_t();
    if (frame == nullptr) {
        return GALAY_OUT_OF_MEMORY;
    }
    frame->type = type;
    frame->flags = data[4];
    frame->stream_id = stream_id;
    frame->payload.assign(data + GALAY_HTTP2_FRAME_HEADER_LENGTH,
                          data + GALAY_HTTP2_FRAME_HEADER_LENGTH + len);
    *out = frame;
    return GALAY_OK;
}

galay_http2_frame_type_t galay_http2_frame_type(const galay_http2_frame_t* frame)
{
    return frame == nullptr ? GALAY_HTTP2_FRAME_DATA : frame->type;
}

uint32_t galay_http2_frame_stream_id(const galay_http2_frame_t* frame)
{
    return frame == nullptr ? 0 : frame->stream_id;
}

galay_status_t galay_http2_ping_frame_opaque(const galay_http2_frame_t* frame, uint8_t out[8])
{
    if (frame == nullptr || out == nullptr || frame->type != GALAY_HTTP2_FRAME_PING ||
        frame->payload.size() != 8) {
        return GALAY_INVALID_ARGUMENT;
    }
    std::memcpy(out, frame->payload.data(), 8);
    return GALAY_OK;
}

galay_status_t galay_http2_headers_create(galay_http2_headers_t** out)
{
    if (out == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    *out = new (std::nothrow) galay_http2_headers_t();
    return *out == nullptr ? GALAY_OUT_OF_MEMORY : GALAY_OK;
}

void galay_http2_headers_destroy(galay_http2_headers_t* headers)
{
    delete headers;
}

galay_status_t galay_http2_headers_add(galay_http2_headers_t* headers, const char* name,
                                       const char* value)
{
    if (headers == nullptr || name == nullptr || value == nullptr || name[0] == '\0') {
        return GALAY_INVALID_ARGUMENT;
    }
    headers->entries.push_back({name, value});
    return GALAY_OK;
}

size_t galay_http2_headers_count(const galay_http2_headers_t* headers)
{
    return headers == nullptr ? 0 : headers->entries.size();
}

galay_status_t galay_http2_headers_get(const galay_http2_headers_t* headers, size_t index,
                                       const char** name, const char** value)
{
    if (headers == nullptr || name == nullptr || value == nullptr || index >= headers->entries.size()) {
        return GALAY_INVALID_ARGUMENT;
    }
    *name = headers->entries[index].name.c_str();
    *value = headers->entries[index].value.c_str();
    return GALAY_OK;
}

galay_status_t galay_http2_hpack_encode(const galay_http2_headers_t* headers, uint8_t* out,
                                        size_t* out_len)
{
    if (headers == nullptr || out_len == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    size_t need = 0;
    for (const auto& entry : headers->entries) {
        if (entry.name.size() > 255 || entry.value.size() > 255) {
            return GALAY_INVALID_ARGUMENT;
        }
        need += 3 + entry.name.size() + entry.value.size();
    }
    if (out == nullptr || *out_len < need) {
        *out_len = need;
        return GALAY_OUT_OF_MEMORY;
    }
    size_t pos = 0;
    for (const auto& entry : headers->entries) {
        out[pos++] = 0x40;
        out[pos++] = static_cast<uint8_t>(entry.name.size());
        std::memcpy(out + pos, entry.name.data(), entry.name.size());
        pos += entry.name.size();
        out[pos++] = static_cast<uint8_t>(entry.value.size());
        std::memcpy(out + pos, entry.value.data(), entry.value.size());
        pos += entry.value.size();
    }
    *out_len = pos;
    return GALAY_OK;
}

galay_status_t galay_http2_hpack_decode(const uint8_t* data, size_t data_len,
                                        galay_http2_headers_t** out)
{
    if (out != nullptr) {
        *out = nullptr;
    }
    if (data == nullptr || out == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    auto* headers = new (std::nothrow) galay_http2_headers_t();
    if (headers == nullptr) {
        return GALAY_OUT_OF_MEMORY;
    }
    size_t pos = 0;
    while (pos < data_len) {
        if (data[pos++] != 0x40 || pos >= data_len) {
            delete headers;
            return GALAY_PROTOCOL_ERROR;
        }
        const size_t name_len = data[pos++];
        if (pos + name_len >= data_len) {
            delete headers;
            return GALAY_PROTOCOL_ERROR;
        }
        std::string name(reinterpret_cast<const char*>(data + pos), name_len);
        pos += name_len;
        const size_t value_len = data[pos++];
        if (pos + value_len > data_len) {
            delete headers;
            return GALAY_PROTOCOL_ERROR;
        }
        std::string value(reinterpret_cast<const char*>(data + pos), value_len);
        pos += value_len;
        headers->entries.push_back({name, value});
    }
    *out = headers;
    return GALAY_OK;
}

}
