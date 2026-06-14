#include "ws_helper.h"

#include <random>

namespace galay::websocket
{

namespace {

void fillMaskingKey(uint8_t masking_key[4])
{
    thread_local static std::random_device rd;
    thread_local static std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 255);
    for (int i = 0; i < 4; ++i) {
        masking_key[i] = static_cast<uint8_t>(dis(gen));
    }
}

} // namespace

size_t wsFrameHeaderLength(uint64_t payload_len, bool use_mask)
{
    size_t header_len = 2;
    if (payload_len >= 126 && payload_len <= 0xFFFF) {
        header_len += 2;
    } else if (payload_len > 0xFFFF) {
        header_len += 8;
    }
    if (use_mask) {
        header_len += 4;
    }
    return header_len;
}

void appendWsFrameHeader(std::string& out,
                         WsOpcode opcode,
                         bool fin,
                         bool rsv1,
                         bool rsv2,
                         bool rsv3,
                         uint64_t payload_len,
                         bool use_mask,
                         uint8_t masking_key[4])
{
    uint8_t byte1 = 0;
    if (fin) byte1 |= 0x80;
    if (rsv1) byte1 |= 0x40;
    if (rsv2) byte1 |= 0x20;
    if (rsv3) byte1 |= 0x10;
    byte1 |= static_cast<uint8_t>(opcode) & 0x0F;
    out.push_back(static_cast<char>(byte1));

    uint8_t byte2 = 0;
    if (use_mask) byte2 |= 0x80;

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

    if (use_mask) {
        fillMaskingKey(masking_key);
        for (int i = 0; i < 4; ++i) {
            out.push_back(static_cast<char>(masking_key[i]));
        }
    }
}

void appendWsFrameHeader(std::string& out,
                         const WsFrame& frame,
                         uint64_t payload_len,
                         bool use_mask,
                         uint8_t masking_key[4])
{
    appendWsFrameHeader(out,
                        frame.header.opcode,
                        frame.header.fin,
                        frame.header.rsv1,
                        frame.header.rsv2,
                        frame.header.rsv3,
                        payload_len,
                        use_mask,
                        masking_key);
}

std::string buildWsClosePayload(WsCloseCode code, const std::string& reason)
{
    std::string payload;
    payload.reserve(2 + reason.size());
    payload.push_back((static_cast<uint16_t>(code) >> 8) & 0xFF);
    payload.push_back(static_cast<uint16_t>(code) & 0xFF);
    payload += reason;
    return payload;
}

} // namespace galay::websocket
