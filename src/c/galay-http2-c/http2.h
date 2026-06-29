#ifndef GALAY_C_HTTP2_HTTP2_H
#define GALAY_C_HTTP2_HTTP2_H

#include <galay/c/galay-common-c/common/galay_c_error.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GALAY_HTTP2_FRAME_HEADER_LENGTH 9u

typedef enum galay_http2_frame_type_t {
    GALAY_HTTP2_FRAME_DATA = 0,
    GALAY_HTTP2_FRAME_HEADERS = 1,
    GALAY_HTTP2_FRAME_SETTINGS = 4,
    GALAY_HTTP2_FRAME_PING = 6
} galay_http2_frame_type_t;

typedef enum galay_http2_settings_id_t {
    GALAY_HTTP2_SETTINGS_ENABLE_PUSH = 2,
    GALAY_HTTP2_SETTINGS_INITIAL_WINDOW_SIZE = 4,
    GALAY_HTTP2_SETTINGS_MAX_FRAME_SIZE = 5
} galay_http2_settings_id_t;

typedef struct galay_http2_frame_t galay_http2_frame_t;
typedef struct galay_http2_headers_t galay_http2_headers_t;

const char* galay_http2_get_error(galay_status_t status);
galay_status_t galay_http2_stream_id_validate(uint32_t stream_id, galay_bool_t allow_zero);
galay_status_t galay_http2_settings_value_validate(galay_http2_settings_id_t id, uint32_t value);
galay_status_t galay_http2_ping_frame_create(const uint8_t opaque[8], galay_bool_t ack,
                                             galay_http2_frame_t** out);
void galay_http2_frame_destroy(galay_http2_frame_t* frame);
galay_status_t galay_http2_frame_encode(const galay_http2_frame_t* frame, uint8_t* out,
                                        size_t* out_len);
galay_status_t galay_http2_frame_decode(const uint8_t* data, size_t data_len,
                                        galay_http2_frame_t** out);
galay_http2_frame_type_t galay_http2_frame_type(const galay_http2_frame_t* frame);
uint32_t galay_http2_frame_stream_id(const galay_http2_frame_t* frame);
galay_status_t galay_http2_ping_frame_opaque(const galay_http2_frame_t* frame, uint8_t out[8]);

galay_status_t galay_http2_headers_create(galay_http2_headers_t** out);
void galay_http2_headers_destroy(galay_http2_headers_t* headers);
galay_status_t galay_http2_headers_add(galay_http2_headers_t* headers, const char* name,
                                       const char* value);
size_t galay_http2_headers_count(const galay_http2_headers_t* headers);
galay_status_t galay_http2_headers_get(const galay_http2_headers_t* headers, size_t index,
                                       const char** name, const char** value);
galay_status_t galay_http2_hpack_encode(const galay_http2_headers_t* headers, uint8_t* out,
                                        size_t* out_len);
galay_status_t galay_http2_hpack_decode(const uint8_t* data, size_t data_len,
                                        galay_http2_headers_t** out);

#ifdef __cplusplus
}
#endif

#endif
