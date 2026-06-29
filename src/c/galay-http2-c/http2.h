#ifndef GALAY_C_HTTP2_HTTP2_H
#define GALAY_C_HTTP2_HTTP2_H

#include <galay/c/galay-common-c/common/galay_c_error.h>
#include <galay/c/galay-kernel-c/coro-c/coro_result_c.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GALAY_HTTP2_FRAME_HEADER_LENGTH 9u

typedef enum galay_http2_frame_type_t {
    GALAY_HTTP2_FRAME_DATA = 0,
    GALAY_HTTP2_FRAME_HEADERS = 1,
    GALAY_HTTP2_FRAME_RST_STREAM = 3,
    GALAY_HTTP2_FRAME_SETTINGS = 4,
    GALAY_HTTP2_FRAME_PING = 6,
    GALAY_HTTP2_FRAME_GOAWAY = 7,
    GALAY_HTTP2_FRAME_WINDOW_UPDATE = 8
} galay_http2_frame_type_t;

typedef enum galay_http2_settings_id_t {
    GALAY_HTTP2_SETTINGS_ENABLE_PUSH = 2,
    GALAY_HTTP2_SETTINGS_INITIAL_WINDOW_SIZE = 4,
    GALAY_HTTP2_SETTINGS_MAX_FRAME_SIZE = 5
} galay_http2_settings_id_t;

typedef enum galay_http2_error_code_t {
    GALAY_HTTP2_ERROR_NONE = 0,
    GALAY_HTTP2_ERROR_PROTOCOL = 1,
    GALAY_HTTP2_ERROR_INTERNAL = 2,
    GALAY_HTTP2_ERROR_FLOW_CONTROL = 3,
    GALAY_HTTP2_ERROR_STREAM_CLOSED = 5,
    GALAY_HTTP2_ERROR_CANCEL = 8,
    GALAY_HTTP2_ERROR_SETTINGS_ACK = 100,
    GALAY_HTTP2_ERROR_STREAM_RESET = 101,
    GALAY_HTTP2_ERROR_GOAWAY = 102,
    GALAY_HTTP2_ERROR_IO = 103,
    GALAY_HTTP2_ERROR_TIMEOUT = 104
} galay_http2_error_code_t;

typedef enum galay_http2_stream_state_t {
    GALAY_HTTP2_STREAM_IDLE = 0,
    GALAY_HTTP2_STREAM_OPEN = 1,
    GALAY_HTTP2_STREAM_HALF_CLOSED_LOCAL = 2,
    GALAY_HTTP2_STREAM_HALF_CLOSED_REMOTE = 3,
    GALAY_HTTP2_STREAM_CLOSED = 4
} galay_http2_stream_state_t;

typedef struct galay_http2_config_t {
    const char* host;
    uint16_t port;
    uint32_t initial_window_size;
    uint32_t max_frame_size;
    uint32_t max_concurrent_streams;
} galay_http2_config_t;

typedef struct galay_http2_frame_t galay_http2_frame_t;
typedef struct galay_http2_headers_t galay_http2_headers_t;
typedef struct galay_http2_client_t galay_http2_client_t;
typedef struct galay_http2_server_t galay_http2_server_t;
typedef struct galay_http2_conn_t galay_http2_conn_t;
typedef struct galay_http2_stream_t galay_http2_stream_t;

const char* galay_http2_get_error(galay_status_t status);
const char* galay_http2_error_code_get_error(galay_http2_error_code_t error);
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

/**
 * @brief 返回 h2c C runtime 默认配置。
 * @return 默认监听/连接到 127.0.0.1:0，初始窗口 65535，最大帧 16384。
 */
galay_http2_config_t galay_http2_config_default(void);

galay_status_t galay_http2_client_create(const galay_http2_config_t* config,
                                         galay_http2_client_t** out);
void galay_http2_client_destroy(galay_http2_client_t* client);
C_IOResult galay_http2_client_connect(galay_http2_client_t* client, int64_t timeout_ms);
galay_http2_conn_t* galay_http2_client_conn(galay_http2_client_t* client);
C_IOResult galay_http2_client_open_stream(galay_http2_client_t* client,
                                          const galay_http2_headers_t* headers,
                                          galay_bool_t end_stream,
                                          galay_http2_stream_t** out_stream,
                                          int64_t timeout_ms);

galay_status_t galay_http2_server_create(const galay_http2_config_t* config,
                                         galay_http2_server_t** out);
void galay_http2_server_destroy(galay_http2_server_t* server);
C_IOResult galay_http2_server_listen(galay_http2_server_t* server, uint16_t* out_port);
C_IOResult galay_http2_server_accept(galay_http2_server_t* server,
                                     galay_http2_conn_t** out_conn,
                                     int64_t timeout_ms);
C_IOResult galay_http2_server_stop(galay_http2_server_t* server, int64_t timeout_ms);

galay_status_t galay_http2_conn_destroy(galay_http2_conn_t* conn);
galay_bool_t galay_http2_conn_settings_ack_received(const galay_http2_conn_t* conn);
C_IOResult galay_http2_conn_accept_stream(galay_http2_conn_t* conn,
                                          galay_http2_stream_t** out_stream,
                                          int64_t timeout_ms);
C_IOResult galay_http2_conn_read_control(galay_http2_conn_t* conn, int64_t timeout_ms);
C_IOResult galay_http2_conn_send_goaway(galay_http2_conn_t* conn,
                                        uint32_t last_stream_id,
                                        galay_http2_error_code_t error,
                                        int64_t timeout_ms);
C_IOResult galay_http2_conn_send_window_update(galay_http2_conn_t* conn,
                                               galay_http2_stream_t* stream,
                                               uint32_t increment,
                                               int64_t timeout_ms);

galay_status_t galay_http2_stream_destroy(galay_http2_stream_t* stream);
uint32_t galay_http2_stream_id(const galay_http2_stream_t* stream);
galay_http2_stream_state_t galay_http2_stream_state(const galay_http2_stream_t* stream);
C_IOResult galay_http2_stream_read_headers(galay_http2_stream_t* stream,
                                           galay_http2_headers_t** out_headers,
                                           int64_t timeout_ms);
C_IOResult galay_http2_stream_write_headers(galay_http2_stream_t* stream,
                                            const galay_http2_headers_t* headers,
                                            galay_bool_t end_stream,
                                            int64_t timeout_ms);
C_IOResult galay_http2_stream_read_data(galay_http2_stream_t* stream,
                                        char* out,
                                        size_t out_len,
                                        size_t* read_len,
                                        galay_bool_t* end_stream,
                                        int64_t timeout_ms);
C_IOResult galay_http2_stream_write_data(galay_http2_stream_t* stream,
                                         const char* data,
                                         size_t data_len,
                                         galay_bool_t end_stream,
                                         int64_t timeout_ms);
C_IOResult galay_http2_stream_reset(galay_http2_stream_t* stream,
                                    galay_http2_error_code_t error,
                                    int64_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif
