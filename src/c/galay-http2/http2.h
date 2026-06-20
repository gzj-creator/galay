/**
 * @file http2.h
 * @brief Galay HTTP/2 C ABI helpers for frame and HPACK codecs.
 */

#ifndef GALAY_C_HTTP2_HTTP2_H
#define GALAY_C_HTTP2_HTTP2_H

#include <galay/c/galay-c/common/galay_c_error.h>

GALAY_C_BEGIN_DECLS

enum {
    GALAY_HTTP2_FRAME_HEADER_LENGTH = 9,
    GALAY_HTTP2_MAX_FRAME_SIZE = 16777215u
};

typedef enum galay_http2_frame_type {
    GALAY_HTTP2_FRAME_DATA = 0x0,
    GALAY_HTTP2_FRAME_HEADERS = 0x1,
    GALAY_HTTP2_FRAME_PRIORITY = 0x2,
    GALAY_HTTP2_FRAME_RST_STREAM = 0x3,
    GALAY_HTTP2_FRAME_SETTINGS = 0x4,
    GALAY_HTTP2_FRAME_PUSH_PROMISE = 0x5,
    GALAY_HTTP2_FRAME_PING = 0x6,
    GALAY_HTTP2_FRAME_GOAWAY = 0x7,
    GALAY_HTTP2_FRAME_WINDOW_UPDATE = 0x8,
    GALAY_HTTP2_FRAME_CONTINUATION = 0x9,
    GALAY_HTTP2_FRAME_UNKNOWN = 0xff
} galay_http2_frame_type_t;

typedef enum galay_http2_settings_id {
    GALAY_HTTP2_SETTINGS_HEADER_TABLE_SIZE = 0x1,
    GALAY_HTTP2_SETTINGS_ENABLE_PUSH = 0x2,
    GALAY_HTTP2_SETTINGS_MAX_CONCURRENT_STREAMS = 0x3,
    GALAY_HTTP2_SETTINGS_INITIAL_WINDOW_SIZE = 0x4,
    GALAY_HTTP2_SETTINGS_MAX_FRAME_SIZE = 0x5,
    GALAY_HTTP2_SETTINGS_MAX_HEADER_LIST_SIZE = 0x6
} galay_http2_settings_id_t;

typedef struct galay_http2_frame galay_http2_frame_t;
typedef struct galay_http2_headers galay_http2_headers_t;

/**
 * @brief Create a PING frame.
 * @param opaque_data exactly 8 bytes copied into the frame.
 * @param ack whether to set the ACK flag.
 * @param out_frame receives an owned frame handle; destroy with galay_http2_frame_destroy.
 * @return GALAY_OK, GALAY_INVALID_ARGUMENT, or GALAY_OUT_OF_MEMORY.
 */
GALAY_C_API galay_status_t galay_http2_ping_frame_create(const uint8_t* opaque_data,
                                                         galay_bool_t ack,
                                                         galay_http2_frame_t** out_frame);

/**
 * @brief Destroy an owned frame handle. NULL is accepted.
 */
GALAY_C_API void galay_http2_frame_destroy(galay_http2_frame_t* frame);

/**
 * @brief Encode a frame into caller-owned bytes.
 * @param frame non-NULL frame handle.
 * @param out_bytes destination buffer.
 * @param inout_length on input capacity; on output required/encoded length.
 * @return GALAY_OK or a failure status. If capacity is too small, returns
 *         GALAY_INVALID_ARGUMENT and writes the required length.
 */
GALAY_C_API galay_status_t galay_http2_frame_encode(const galay_http2_frame_t* frame,
                                                    uint8_t* out_bytes,
                                                    size_t* inout_length);

/**
 * @brief Decode exactly one complete HTTP/2 frame.
 * @param bytes serialized frame bytes.
 * @param length byte count. Must match the frame header payload length exactly.
 * @param out_frame receives an owned frame handle; destroy with galay_http2_frame_destroy.
 * @return GALAY_OK, GALAY_INVALID_ARGUMENT, GALAY_PROTOCOL_ERROR, or GALAY_OUT_OF_MEMORY.
 */
GALAY_C_API galay_status_t galay_http2_frame_decode(const uint8_t* bytes,
                                                    size_t length,
                                                    galay_http2_frame_t** out_frame);

GALAY_C_API galay_http2_frame_type_t galay_http2_frame_type(const galay_http2_frame_t* frame);
GALAY_C_API uint32_t galay_http2_frame_stream_id(const galay_http2_frame_t* frame);
GALAY_C_API galay_status_t galay_http2_ping_frame_opaque(const galay_http2_frame_t* frame,
                                                         uint8_t out_opaque_data[8]);

/**
 * @brief Create a mutable HPACK header list.
 */
GALAY_C_API galay_status_t galay_http2_headers_create(galay_http2_headers_t** out_headers);

/**
 * @brief Destroy an owned HPACK header list. NULL is accepted.
 */
GALAY_C_API void galay_http2_headers_destroy(galay_http2_headers_t* headers);

/**
 * @brief Append one header field. Name and value are copied.
 */
GALAY_C_API galay_status_t galay_http2_headers_add(galay_http2_headers_t* headers,
                                                   const char* name,
                                                   const char* value);

GALAY_C_API size_t galay_http2_headers_count(const galay_http2_headers_t* headers);

/**
 * @brief Read one header field.
 * @note Returned pointers are borrowed and valid until the list is mutated or destroyed.
 */
GALAY_C_API galay_status_t galay_http2_headers_get(const galay_http2_headers_t* headers,
                                                   size_t index,
                                                   const char** out_name,
                                                   const char** out_value);

/**
 * @brief Statelessly HPACK-encode a header list into caller-owned bytes.
 * @param inout_length on input capacity; on output required/encoded length.
 */
GALAY_C_API galay_status_t galay_http2_hpack_encode(const galay_http2_headers_t* headers,
                                                    uint8_t* out_bytes,
                                                    size_t* inout_length);

/**
 * @brief HPACK-decode a header block into an owned header list.
 */
GALAY_C_API galay_status_t galay_http2_hpack_decode(const uint8_t* bytes,
                                                    size_t length,
                                                    galay_http2_headers_t** out_headers);

/**
 * @brief Validate a stream id for a C caller.
 * @param allow_zero pass GALAY_TRUE for connection-level frames.
 */
GALAY_C_API galay_status_t galay_http2_stream_id_validate(uint32_t stream_id,
                                                          galay_bool_t allow_zero);

/**
 * @brief Validate one SETTINGS value against HTTP/2 RFC boundaries.
 */
GALAY_C_API galay_status_t galay_http2_settings_value_validate(galay_http2_settings_id_t id,
                                                               uint32_t value);

GALAY_C_END_DECLS

#endif /* GALAY_C_HTTP2_HTTP2_H */
