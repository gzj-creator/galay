#include <galay/c/galay-ws-c/ws.h>

#include <cstring>

static bool valid_opcode(galay_ws_opcode_t opcode)
{
    return opcode == GALAY_WS_OPCODE_CONTINUATION || opcode == GALAY_WS_OPCODE_TEXT ||
        opcode == GALAY_WS_OPCODE_BINARY || opcode == GALAY_WS_OPCODE_CLOSE ||
        opcode == GALAY_WS_OPCODE_PING || opcode == GALAY_WS_OPCODE_PONG;
}

extern "C" {

const char* galay_ws_get_error(galay_ws_error_t error)
{
    switch (error) {
        case GALAY_WS_ERROR_NONE:
            return "none";
        case GALAY_WS_ERROR_INCOMPLETE:
            return "incomplete";
        case GALAY_WS_ERROR_INVALID_OPCODE:
            return "invalid opcode";
    }
    return "unknown";
}

galay_status_t galay_ws_encoded_size(size_t payload_len, galay_bool_t masked,
                                     size_t* encoded_size)
{
    if (encoded_size == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    size_t header = 2;
    if (payload_len >= 126 && payload_len <= 65535) {
        header += 2;
    } else if (payload_len > 65535) {
        header += 8;
    }
    if (masked == GALAY_TRUE) {
        header += 4;
    }
    *encoded_size = header + payload_len;
    return GALAY_OK;
}

galay_status_t galay_ws_apply_mask(uint8_t* data, size_t len, const uint8_t mask_key[4])
{
    if ((data == nullptr && len != 0) || mask_key == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    for (size_t i = 0; i < len; ++i) {
        data[i] ^= mask_key[i % 4];
    }
    return GALAY_OK;
}

galay_status_t galay_ws_encode_frame(galay_ws_opcode_t opcode, const uint8_t* payload,
                                     size_t payload_len, galay_bool_t fin,
                                     const uint8_t mask_key[4], uint8_t* out,
                                     size_t out_len, size_t* written)
{
    if (written != nullptr) {
        *written = 0;
    }
    if (!valid_opcode(opcode) || out == nullptr || written == nullptr ||
        (payload == nullptr && payload_len != 0)) {
        return GALAY_INVALID_ARGUMENT;
    }
    const bool masked = mask_key != nullptr;
    size_t need = 0;
    const galay_status_t size_status = galay_ws_encoded_size(payload_len, masked ? GALAY_TRUE : GALAY_FALSE, &need);
    if (size_status != GALAY_OK) {
        return size_status;
    }
    if (out_len < need) {
        return GALAY_OUT_OF_MEMORY;
    }
    size_t pos = 0;
    out[pos++] = static_cast<uint8_t>((fin == GALAY_TRUE ? 0x80U : 0U) | static_cast<uint8_t>(opcode));
    if (payload_len < 126) {
        out[pos++] = static_cast<uint8_t>((masked ? 0x80U : 0U) | payload_len);
    } else if (payload_len <= 65535) {
        out[pos++] = static_cast<uint8_t>((masked ? 0x80U : 0U) | 126U);
        out[pos++] = static_cast<uint8_t>((payload_len >> 8U) & 0xFFU);
        out[pos++] = static_cast<uint8_t>(payload_len & 0xFFU);
    } else {
        out[pos++] = static_cast<uint8_t>((masked ? 0x80U : 0U) | 127U);
        for (int i = 7; i >= 0; --i) {
            out[pos++] = static_cast<uint8_t>((static_cast<uint64_t>(payload_len) >> (i * 8)) & 0xFFU);
        }
    }
    if (masked) {
        std::memcpy(out + pos, mask_key, 4);
        pos += 4;
    }
    for (size_t i = 0; i < payload_len; ++i) {
        out[pos + i] = masked ? static_cast<uint8_t>(payload[i] ^ mask_key[i % 4]) : payload[i];
    }
    pos += payload_len;
    *written = pos;
    return GALAY_OK;
}

galay_status_t galay_ws_decode_frame(const uint8_t* data, size_t data_len,
                                     galay_bool_t expect_masked,
                                     galay_ws_frame_t* frame, uint8_t* payload_out,
                                     size_t payload_out_len, size_t* consumed,
                                     galay_ws_error_t* ws_error)
{
    if (consumed != nullptr) {
        *consumed = 0;
    }
    if (ws_error != nullptr) {
        *ws_error = GALAY_WS_ERROR_NONE;
    }
    if (data == nullptr || frame == nullptr || consumed == nullptr || ws_error == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    if (data_len < 2) {
        *ws_error = GALAY_WS_ERROR_INCOMPLETE;
        return GALAY_PROTOCOL_ERROR;
    }
    const auto opcode = static_cast<galay_ws_opcode_t>(data[0] & 0x0FU);
    if (!valid_opcode(opcode)) {
        *ws_error = GALAY_WS_ERROR_INVALID_OPCODE;
        return GALAY_PROTOCOL_ERROR;
    }
    size_t pos = 2;
    bool masked = (data[1] & 0x80U) != 0;
    uint64_t payload_len = data[1] & 0x7FU;
    if (payload_len == 126) {
        if (data_len < pos + 2) {
            *ws_error = GALAY_WS_ERROR_INCOMPLETE;
            return GALAY_PROTOCOL_ERROR;
        }
        payload_len = (static_cast<uint64_t>(data[pos]) << 8U) | data[pos + 1];
        pos += 2;
    } else if (payload_len == 127) {
        if (data_len < pos + 8) {
            *ws_error = GALAY_WS_ERROR_INCOMPLETE;
            return GALAY_PROTOCOL_ERROR;
        }
        payload_len = 0;
        for (size_t i = 0; i < 8; ++i) {
            payload_len = (payload_len << 8U) | data[pos + i];
        }
        pos += 8;
    }
    if (expect_masked == GALAY_TRUE && !masked) {
        *ws_error = GALAY_WS_ERROR_INCOMPLETE;
        return GALAY_PROTOCOL_ERROR;
    }
    uint8_t mask[4] = {0, 0, 0, 0};
    if (masked) {
        if (data_len < pos + 4) {
            *ws_error = GALAY_WS_ERROR_INCOMPLETE;
            return GALAY_PROTOCOL_ERROR;
        }
        std::memcpy(mask, data + pos, 4);
        pos += 4;
    }
    if (data_len < pos + payload_len || payload_out_len < payload_len ||
        (payload_out == nullptr && payload_len != 0)) {
        *ws_error = GALAY_WS_ERROR_INCOMPLETE;
        return GALAY_PROTOCOL_ERROR;
    }
    frame->fin = (data[0] & 0x80U) != 0 ? GALAY_TRUE : GALAY_FALSE;
    frame->opcode = opcode;
    frame->masked = masked ? GALAY_TRUE : GALAY_FALSE;
    frame->payload_len = payload_len;
    std::memcpy(frame->masking_key, mask, 4);
    for (size_t i = 0; i < payload_len; ++i) {
        payload_out[i] = masked ? static_cast<uint8_t>(data[pos + i] ^ mask[i % 4]) : data[pos + i];
    }
    *consumed = pos + static_cast<size_t>(payload_len);
    return GALAY_OK;
}

}
