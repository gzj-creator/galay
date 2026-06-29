#ifndef GALAY_C_WS_WS_H
#define GALAY_C_WS_WS_H

#include <galay/c/galay-common-c/common/galay_c_error.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum galay_ws_opcode_t {
    GALAY_WS_OPCODE_CONTINUATION = 0x0,
    GALAY_WS_OPCODE_TEXT = 0x1,
    GALAY_WS_OPCODE_BINARY = 0x2,
    GALAY_WS_OPCODE_CLOSE = 0x8,
    GALAY_WS_OPCODE_PING = 0x9,
    GALAY_WS_OPCODE_PONG = 0xA
} galay_ws_opcode_t;

typedef enum galay_ws_close_code_t {
    GALAY_WS_CLOSE_NORMAL = 1000,
    GALAY_WS_CLOSE_PROTOCOL_ERROR = 1002
} galay_ws_close_code_t;

typedef enum galay_ws_error_t {
    GALAY_WS_ERROR_NONE = 0,
    GALAY_WS_ERROR_INCOMPLETE = 1,
    GALAY_WS_ERROR_INVALID_OPCODE = 2
} galay_ws_error_t;

typedef struct galay_ws_frame_t {
    galay_bool_t fin;
    galay_ws_opcode_t opcode;
    galay_bool_t masked;
    uint64_t payload_len;
    uint8_t masking_key[4];
} galay_ws_frame_t;

const char* galay_ws_get_error(galay_ws_error_t error);
galay_status_t galay_ws_encoded_size(size_t payload_len, galay_bool_t masked,
                                     size_t* encoded_size);
galay_status_t galay_ws_apply_mask(uint8_t* data, size_t len, const uint8_t mask_key[4]);
galay_status_t galay_ws_encode_frame(galay_ws_opcode_t opcode, const uint8_t* payload,
                                     size_t payload_len, galay_bool_t fin,
                                     const uint8_t mask_key[4], uint8_t* out,
                                     size_t out_len, size_t* written);
galay_status_t galay_ws_decode_frame(const uint8_t* data, size_t data_len,
                                     galay_bool_t expect_masked,
                                     galay_ws_frame_t* frame, uint8_t* payload_out,
                                     size_t payload_out_len, size_t* consumed,
                                     galay_ws_error_t* ws_error);

#ifdef __cplusplus
}
#endif

#endif
