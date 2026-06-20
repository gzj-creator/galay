/**
 * @file ws.h
 * @brief Galay WebSocket C ABI 帧编解码接口。
 */

#ifndef GALAY_C_WS_WS_H
#define GALAY_C_WS_WS_H

#include <galay/c/galay-c/common/galay_c_error.h>

GALAY_C_BEGIN_DECLS

/**
 * @brief RFC 6455 WebSocket opcode。
 */
typedef enum galay_ws_opcode {
    GALAY_WS_OPCODE_CONTINUATION = 0x0,
    GALAY_WS_OPCODE_TEXT = 0x1,
    GALAY_WS_OPCODE_BINARY = 0x2,
    GALAY_WS_OPCODE_CLOSE = 0x8,
    GALAY_WS_OPCODE_PING = 0x9,
    GALAY_WS_OPCODE_PONG = 0xA
} galay_ws_opcode_t;

/**
 * @brief RFC 6455 close code。
 */
typedef enum galay_ws_close_code {
    GALAY_WS_CLOSE_NORMAL = 1000,
    GALAY_WS_CLOSE_GOING_AWAY = 1001,
    GALAY_WS_CLOSE_PROTOCOL_ERROR = 1002,
    GALAY_WS_CLOSE_UNSUPPORTED_DATA = 1003,
    GALAY_WS_CLOSE_NO_STATUS_RECEIVED = 1005,
    GALAY_WS_CLOSE_ABNORMAL_CLOSURE = 1006,
    GALAY_WS_CLOSE_INVALID_PAYLOAD = 1007,
    GALAY_WS_CLOSE_POLICY_VIOLATION = 1008,
    GALAY_WS_CLOSE_MESSAGE_TOO_BIG = 1009,
    GALAY_WS_CLOSE_MANDATORY_EXTENSION = 1010,
    GALAY_WS_CLOSE_INTERNAL_ERROR = 1011,
    GALAY_WS_CLOSE_TLS_HANDSHAKE = 1015
} galay_ws_close_code_t;

/**
 * @brief WebSocket 协议层解析错误。
 */
typedef enum galay_ws_error {
    GALAY_WS_ERROR_NONE = 0,
    GALAY_WS_ERROR_INCOMPLETE = 1,
    GALAY_WS_ERROR_INVALID_FRAME = 2,
    GALAY_WS_ERROR_INVALID_OPCODE = 3,
    GALAY_WS_ERROR_INVALID_PAYLOAD_LENGTH = 4,
    GALAY_WS_ERROR_CONTROL_FRAME_TOO_LARGE = 5,
    GALAY_WS_ERROR_CONTROL_FRAME_FRAGMENTED = 6,
    GALAY_WS_ERROR_INVALID_UTF8 = 7,
    GALAY_WS_ERROR_PROTOCOL = 8,
    GALAY_WS_ERROR_MESSAGE_TOO_LARGE = 9,
    GALAY_WS_ERROR_INVALID_CLOSE_CODE = 10,
    GALAY_WS_ERROR_RESERVED_BITS_SET = 11,
    GALAY_WS_ERROR_MASK_REQUIRED = 12,
    GALAY_WS_ERROR_MASK_NOT_ALLOWED = 13,
    GALAY_WS_ERROR_UNKNOWN = 255
} galay_ws_error_t;

/**
 * @brief 解码后的 WebSocket 帧元数据。
 * @note payload 数据由调用方提供的输出缓冲区接收，本结构不拥有 payload。
 */
typedef struct galay_ws_frame {
    galay_bool_t fin;
    galay_ws_opcode_t opcode;
    galay_bool_t masked;
    uint64_t payload_len;
    uint8_t masking_key[4];
} galay_ws_frame_t;

/**
 * @brief 返回 WebSocket 协议错误的静态字符串。
 */
GALAY_C_API const char* galay_ws_error_string(galay_ws_error_t error);

/**
 * @brief 判断 opcode 是否是 RFC 6455 支持的公开 opcode。
 */
GALAY_C_API galay_bool_t galay_ws_opcode_is_valid(galay_ws_opcode_t opcode);

/**
 * @brief 计算编码 WebSocket 帧所需字节数。
 * @param payload_len payload 长度。
 * @param masked 是否包含 4 字节 masking key。
 * @param out_size 输出编码总长度。
 * @return GALAY_OK 或 GALAY_INVALID_ARGUMENT。
 */
GALAY_C_API galay_status_t galay_ws_encoded_size(uint64_t payload_len,
                                                 galay_bool_t masked,
                                                 size_t* out_size);

/**
 * @brief 编码单个 WebSocket 帧。
 * @param opcode 帧 opcode，必须是公开 opcode。
 * @param payload payload 字节；payload_len 为 0 时可为 NULL。
 * @param payload_len payload 长度。
 * @param fin FIN 位；控制帧必须为 GALAY_TRUE。
 * @param masking_key 非 NULL 时编码为 masked 帧，并使用该 4 字节 key。
 * @param out 输出缓冲区。
 * @param out_capacity 输出缓冲区长度。
 * @param written 实际写入字节数。
 * @return GALAY_OK、GALAY_INVALID_ARGUMENT 或 GALAY_OUT_OF_MEMORY。
 */
GALAY_C_API galay_status_t galay_ws_encode_frame(galay_ws_opcode_t opcode,
                                                 const uint8_t* payload,
                                                 size_t payload_len,
                                                 galay_bool_t fin,
                                                 const uint8_t masking_key[4],
                                                 uint8_t* out,
                                                 size_t out_capacity,
                                                 size_t* written);

/**
 * @brief 解码单个 WebSocket 帧并拷贝解掩码后的 payload。
 * @param data 输入帧字节。
 * @param data_len 输入长度。
 * @param is_server 为 GALAY_TRUE 时按服务端接收规则要求客户端帧带 mask。
 * @param frame 输出帧元数据。
 * @param payload_out payload 输出缓冲区；payload 为 0 字节时可为 NULL。
 * @param payload_out_capacity payload 输出缓冲区长度。
 * @param consumed 成功时输出消费字节数，失败时置 0。
 * @param ws_error 输出 WebSocket 协议错误；可为 NULL。
 * @return GALAY_OK、GALAY_INVALID_ARGUMENT、GALAY_OUT_OF_MEMORY 或 GALAY_PROTOCOL_ERROR。
 */
GALAY_C_API galay_status_t galay_ws_decode_frame(const uint8_t* data,
                                                 size_t data_len,
                                                 galay_bool_t is_server,
                                                 galay_ws_frame_t* frame,
                                                 uint8_t* payload_out,
                                                 size_t payload_out_capacity,
                                                 size_t* consumed,
                                                 galay_ws_error_t* ws_error);

/**
 * @brief 原地应用 WebSocket mask；再次使用同一 key 可恢复原文。
 */
GALAY_C_API galay_status_t galay_ws_apply_mask(uint8_t* data,
                                               size_t len,
                                               const uint8_t masking_key[4]);

GALAY_C_END_DECLS

#endif /* GALAY_C_WS_WS_H */

