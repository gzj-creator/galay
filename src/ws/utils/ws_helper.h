/**
 * @file ws_helper.h
 * @brief WebSocket frame byte helper utilities
 * @author galay-http
 * @version 1.0.0
 */

#ifndef GALAY_WS_HELPER_H
#define GALAY_WS_HELPER_H

#include "ws/protoc/ws_base.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace galay::websocket
{

/**
 * @brief Compute the encoded WebSocket frame header length.
 * @param payload_len Frame payload length.
 * @param use_mask Whether the encoded frame includes a masking key.
 * @return Header length in bytes.
 */
size_t wsFrameHeaderLength(uint64_t payload_len, bool use_mask);

/**
 * @brief Append a serialized WebSocket frame header.
 * @param out Destination buffer; header bytes are appended.
 * @param opcode WebSocket opcode.
 * @param fin FIN bit.
 * @param rsv1 RSV1 bit.
 * @param rsv2 RSV2 bit.
 * @param rsv3 RSV3 bit.
 * @param payload_len Frame payload length.
 * @param use_mask Whether to set MASK and append a generated masking key.
 * @param masking_key Output masking key when use_mask is true.
 */
void appendWsFrameHeader(std::string& out,
                         WsOpcode opcode,
                         bool fin,
                         bool rsv1,
                         bool rsv2,
                         bool rsv3,
                         uint64_t payload_len,
                         bool use_mask,
                         uint8_t masking_key[4]);

/**
 * @brief Append a serialized WebSocket frame header from a WsFrame.
 */
void appendWsFrameHeader(std::string& out,
                         const WsFrame& frame,
                         uint64_t payload_len,
                         bool use_mask,
                         uint8_t masking_key[4]);

/**
 * @brief Build the payload bytes for a WebSocket close frame.
 * @param code Close code encoded in network byte order.
 * @param reason Optional close reason appended after the code.
 * @return Close frame payload bytes.
 */
std::string buildWsClosePayload(WsCloseCode code, const std::string& reason);

} // namespace galay::websocket

#endif // GALAY_WS_HELPER_H
