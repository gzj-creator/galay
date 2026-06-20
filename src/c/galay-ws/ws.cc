#include <galay/c/galay-ws/ws.h>

#include <galay/cpp/galay-ws/protoc/ws_frame.h>
#include <galay/cpp/galay-ws/utils/ws_helper.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <string>
#include <sys/uio.h>

namespace {

using galay::websocket::WsErrorCode;
using galay::websocket::WsFrame;
using galay::websocket::WsFrameParser;
using galay::websocket::WsOpcode;

bool is_valid_opcode(galay_ws_opcode_t opcode) noexcept
{
    switch (opcode) {
    case GALAY_WS_OPCODE_CONTINUATION:
    case GALAY_WS_OPCODE_TEXT:
    case GALAY_WS_OPCODE_BINARY:
    case GALAY_WS_OPCODE_CLOSE:
    case GALAY_WS_OPCODE_PING:
    case GALAY_WS_OPCODE_PONG:
        return true;
    default:
        return false;
    }
}

bool is_control_opcode(galay_ws_opcode_t opcode) noexcept
{
    return opcode == GALAY_WS_OPCODE_CLOSE ||
           opcode == GALAY_WS_OPCODE_PING ||
           opcode == GALAY_WS_OPCODE_PONG;
}

galay_ws_error_t map_ws_error(WsErrorCode code) noexcept
{
    switch (code) {
    case galay::websocket::kWsNoError:
        return GALAY_WS_ERROR_NONE;
    case galay::websocket::kWsIncomplete:
        return GALAY_WS_ERROR_INCOMPLETE;
    case galay::websocket::kWsInvalidFrame:
        return GALAY_WS_ERROR_INVALID_FRAME;
    case galay::websocket::kWsInvalidOpcode:
        return GALAY_WS_ERROR_INVALID_OPCODE;
    case galay::websocket::kWsInvalidPayloadLength:
        return GALAY_WS_ERROR_INVALID_PAYLOAD_LENGTH;
    case galay::websocket::kWsControlFrameTooLarge:
        return GALAY_WS_ERROR_CONTROL_FRAME_TOO_LARGE;
    case galay::websocket::kWsControlFrameFragmented:
        return GALAY_WS_ERROR_CONTROL_FRAME_FRAGMENTED;
    case galay::websocket::kWsInvalidUtf8:
        return GALAY_WS_ERROR_INVALID_UTF8;
    case galay::websocket::kWsProtocolError:
        return GALAY_WS_ERROR_PROTOCOL;
    case galay::websocket::kWsMessageTooLarge:
        return GALAY_WS_ERROR_MESSAGE_TOO_LARGE;
    case galay::websocket::kWsInvalidCloseCode:
        return GALAY_WS_ERROR_INVALID_CLOSE_CODE;
    case galay::websocket::kWsReservedBitsSet:
        return GALAY_WS_ERROR_RESERVED_BITS_SET;
    case galay::websocket::kWsMaskRequired:
        return GALAY_WS_ERROR_MASK_REQUIRED;
    case galay::websocket::kWsMaskNotAllowed:
        return GALAY_WS_ERROR_MASK_NOT_ALLOWED;
    default:
        return GALAY_WS_ERROR_UNKNOWN;
    }
}

void write_ws_error(galay_ws_error_t* out, galay_ws_error_t error) noexcept
{
    if (out != nullptr) {
        *out = error;
    }
}

galay_status_t checked_encoded_size(uint64_t payload_len, bool masked, size_t* out_size) noexcept
{
    if (out_size == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    const size_t header_len = galay::websocket::wsFrameHeaderLength(payload_len, masked);
    if (payload_len > static_cast<uint64_t>(std::numeric_limits<size_t>::max() - header_len)) {
        *out_size = 0;
        return GALAY_OUT_OF_MEMORY;
    }
    *out_size = header_len + static_cast<size_t>(payload_len);
    return GALAY_OK;
}

void append_manual_header(std::string& out,
                          galay_ws_opcode_t opcode,
                          bool fin,
                          uint64_t payload_len,
                          const uint8_t masking_key[4])
{
    uint8_t byte1 = static_cast<uint8_t>(opcode) & 0x0F;
    if (fin) {
        byte1 |= 0x80;
    }
    out.push_back(static_cast<char>(byte1));

    uint8_t byte2 = masking_key != nullptr ? 0x80 : 0;
    if (payload_len < 126) {
        byte2 |= static_cast<uint8_t>(payload_len);
        out.push_back(static_cast<char>(byte2));
    } else if (payload_len <= 0xFFFF) {
        byte2 |= 126;
        out.push_back(static_cast<char>(byte2));
        out.push_back(static_cast<char>((payload_len >> 8) & 0xFF));
        out.push_back(static_cast<char>(payload_len & 0xFF));
    } else {
        byte2 |= 127;
        out.push_back(static_cast<char>(byte2));
        for (int i = 7; i >= 0; --i) {
            out.push_back(static_cast<char>((payload_len >> (i * 8)) & 0xFF));
        }
    }

    if (masking_key != nullptr) {
        out.append(reinterpret_cast<const char*>(masking_key), 4);
    }
}

} // namespace

const char* galay_ws_error_string(galay_ws_error_t error)
{
    switch (error) {
    case GALAY_WS_ERROR_NONE:
        return "none";
    case GALAY_WS_ERROR_INCOMPLETE:
        return "incomplete";
    case GALAY_WS_ERROR_INVALID_FRAME:
        return "invalid frame";
    case GALAY_WS_ERROR_INVALID_OPCODE:
        return "invalid opcode";
    case GALAY_WS_ERROR_INVALID_PAYLOAD_LENGTH:
        return "invalid payload length";
    case GALAY_WS_ERROR_CONTROL_FRAME_TOO_LARGE:
        return "control frame too large";
    case GALAY_WS_ERROR_CONTROL_FRAME_FRAGMENTED:
        return "control frame fragmented";
    case GALAY_WS_ERROR_INVALID_UTF8:
        return "invalid utf8";
    case GALAY_WS_ERROR_PROTOCOL:
        return "protocol error";
    case GALAY_WS_ERROR_MESSAGE_TOO_LARGE:
        return "message too large";
    case GALAY_WS_ERROR_INVALID_CLOSE_CODE:
        return "invalid close code";
    case GALAY_WS_ERROR_RESERVED_BITS_SET:
        return "reserved bits set";
    case GALAY_WS_ERROR_MASK_REQUIRED:
        return "mask required";
    case GALAY_WS_ERROR_MASK_NOT_ALLOWED:
        return "mask not allowed";
    default:
        return "unknown";
    }
}

galay_bool_t galay_ws_opcode_is_valid(galay_ws_opcode_t opcode)
{
    return is_valid_opcode(opcode) ? GALAY_TRUE : GALAY_FALSE;
}

galay_status_t galay_ws_encoded_size(uint64_t payload_len, galay_bool_t masked, size_t* out_size)
{
    try {
        return checked_encoded_size(payload_len, masked != GALAY_FALSE, out_size);
    } catch (...) {
        if (out_size != nullptr) {
            *out_size = 0;
        }
        return GALAY_INTERNAL_ERROR;
    }
}

galay_status_t galay_ws_encode_frame(galay_ws_opcode_t opcode,
                                     const uint8_t* payload,
                                     size_t payload_len,
                                     galay_bool_t fin,
                                     const uint8_t masking_key[4],
                                     uint8_t* out,
                                     size_t out_capacity,
                                     size_t* written)
{
    if (written != nullptr) {
        *written = 0;
    }
    if (!is_valid_opcode(opcode) ||
        (payload_len > 0 && payload == nullptr) ||
        out == nullptr ||
        (is_control_opcode(opcode) && (fin == GALAY_FALSE || payload_len > 125))) {
        return GALAY_INVALID_ARGUMENT;
    }

    try {
        size_t required = 0;
        galay_status_t status = checked_encoded_size(payload_len, masking_key != nullptr, &required);
        if (status != GALAY_OK) {
            return status;
        }
        if (out_capacity < required) {
            return GALAY_OUT_OF_MEMORY;
        }

        std::string encoded;
        encoded.reserve(required);
        append_manual_header(encoded, opcode, fin != GALAY_FALSE, payload_len, masking_key);
        if (payload_len > 0) {
            encoded.append(reinterpret_cast<const char*>(payload), payload_len);
            if (masking_key != nullptr) {
                WsFrameParser::applyMaskBytes(encoded.data() + (encoded.size() - payload_len),
                                              payload_len,
                                              masking_key);
            }
        }

        std::memcpy(out, encoded.data(), encoded.size());
        if (written != nullptr) {
            *written = encoded.size();
        }
        return GALAY_OK;
    } catch (const std::bad_alloc&) {
        return GALAY_OUT_OF_MEMORY;
    } catch (...) {
        return GALAY_INTERNAL_ERROR;
    }
}

galay_status_t galay_ws_decode_frame(const uint8_t* data,
                                     size_t data_len,
                                     galay_bool_t is_server,
                                     galay_ws_frame_t* frame,
                                     uint8_t* payload_out,
                                     size_t payload_out_capacity,
                                     size_t* consumed,
                                     galay_ws_error_t* ws_error)
{
    if (consumed != nullptr) {
        *consumed = 0;
    }
    write_ws_error(ws_error, GALAY_WS_ERROR_NONE);

    if ((data_len > 0 && data == nullptr) || frame == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }

    try {
        struct iovec iov;
        iov.iov_base = const_cast<uint8_t*>(data);
        iov.iov_len = data_len;

        WsFrame cpp_frame;
        auto parsed = WsFrameParser::fromIOVec(&iov, 1, cpp_frame, is_server != GALAY_FALSE);
        if (!parsed.has_value()) {
            write_ws_error(ws_error, map_ws_error(parsed.error().code()));
            return GALAY_PROTOCOL_ERROR;
        }

        if (cpp_frame.payload.size() > payload_out_capacity ||
            (!cpp_frame.payload.empty() && payload_out == nullptr)) {
            write_ws_error(ws_error, GALAY_WS_ERROR_MESSAGE_TOO_LARGE);
            return GALAY_OUT_OF_MEMORY;
        }

        frame->fin = cpp_frame.header.fin ? GALAY_TRUE : GALAY_FALSE;
        frame->opcode = static_cast<galay_ws_opcode_t>(cpp_frame.header.opcode);
        frame->masked = cpp_frame.header.mask ? GALAY_TRUE : GALAY_FALSE;
        frame->payload_len = cpp_frame.payload.size();
        std::copy(std::begin(cpp_frame.header.masking_key),
                  std::end(cpp_frame.header.masking_key),
                  std::begin(frame->masking_key));

        if (!cpp_frame.payload.empty()) {
            std::memcpy(payload_out, cpp_frame.payload.data(), cpp_frame.payload.size());
        }
        if (consumed != nullptr) {
            *consumed = *parsed;
        }
        return GALAY_OK;
    } catch (const std::bad_alloc&) {
        return GALAY_OUT_OF_MEMORY;
    } catch (...) {
        write_ws_error(ws_error, GALAY_WS_ERROR_UNKNOWN);
        return GALAY_INTERNAL_ERROR;
    }
}

galay_status_t galay_ws_apply_mask(uint8_t* data, size_t len, const uint8_t masking_key[4])
{
    if ((len > 0 && data == nullptr) || masking_key == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }

    try {
        WsFrameParser::applyMaskBytes(reinterpret_cast<char*>(data), len, masking_key);
        return GALAY_OK;
    } catch (...) {
        return GALAY_INTERNAL_ERROR;
    }
}

