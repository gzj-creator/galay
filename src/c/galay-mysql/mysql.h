/**
 * @file mysql.h
 * @brief galay-mysql C ABI 封装。
 *
 * @details 该头文件只暴露 C 兼容的 opaque handle、简单值类型和显式错误码。
 *          返回的字节缓冲区由 galay_mysql_buffer_t 持有，调用方需调用
 *          galay_mysql_buffer_destroy 释放。packet view 的 payload 指针借用输入缓冲区。
 */

#ifndef GALAY_C_MYSQL_MYSQL_H
#define GALAY_C_MYSQL_MYSQL_H

#include <galay/c/galay-c/common/galay_c_error.h>

GALAY_C_BEGIN_DECLS

typedef struct galay_mysql_config galay_mysql_config_t;
typedef struct galay_mysql_client galay_mysql_client_t;
typedef struct galay_mysql_buffer galay_mysql_buffer_t;

typedef struct galay_mysql_packet_header {
    uint32_t payload_length; ///< MySQL packet payload 长度。
    uint8_t sequence_id;    ///< MySQL packet 序列号。
} galay_mysql_packet_header_t;

typedef struct galay_mysql_packet_view {
    const unsigned char* payload; ///< 借用输入缓冲区中的 payload 指针。
    uint32_t payload_len;         ///< payload 长度。
    uint8_t sequence_id;          ///< MySQL packet 序列号。
    size_t consumed;              ///< 包头加 payload 消耗的总字节数。
} galay_mysql_packet_view_t;

GALAY_C_API galay_status_t galay_mysql_config_create(galay_mysql_config_t** out);
GALAY_C_API void galay_mysql_config_destroy(galay_mysql_config_t* config);
GALAY_C_API galay_status_t galay_mysql_config_set_host(galay_mysql_config_t* config,
                                                       const char* host);
GALAY_C_API galay_status_t galay_mysql_config_set_port(galay_mysql_config_t* config,
                                                       uint16_t port);
GALAY_C_API galay_status_t galay_mysql_config_set_username(galay_mysql_config_t* config,
                                                           const char* username);
GALAY_C_API galay_status_t galay_mysql_config_set_password(galay_mysql_config_t* config,
                                                           const char* password);
GALAY_C_API galay_status_t galay_mysql_config_set_database(galay_mysql_config_t* config,
                                                           const char* database);
GALAY_C_API galay_status_t galay_mysql_config_set_charset(galay_mysql_config_t* config,
                                                          const char* charset);
GALAY_C_API galay_status_t galay_mysql_config_set_connect_timeout_ms(galay_mysql_config_t* config,
                                                                     uint32_t timeout_ms);
GALAY_C_API galay_status_t galay_mysql_config_host(const galay_mysql_config_t* config,
                                                   const char** host);
GALAY_C_API galay_status_t galay_mysql_config_port(const galay_mysql_config_t* config,
                                                   uint16_t* port);
GALAY_C_API galay_status_t galay_mysql_config_validate(const galay_mysql_config_t* config);

GALAY_C_API void galay_mysql_buffer_destroy(galay_mysql_buffer_t* buffer);
GALAY_C_API galay_status_t galay_mysql_buffer_data(const galay_mysql_buffer_t* buffer,
                                                   const unsigned char** data,
                                                   size_t* data_len);

GALAY_C_API galay_status_t galay_mysql_auth_response_for_plugin(const char* plugin_name,
                                                                const char* password,
                                                                const unsigned char* salt,
                                                                size_t salt_len,
                                                                galay_mysql_buffer_t** out);

GALAY_C_API galay_status_t galay_mysql_parse_packet_header(const void* data,
                                                           size_t data_len,
                                                           galay_mysql_packet_header_t* out);
GALAY_C_API galay_status_t galay_mysql_extract_packet(const void* data,
                                                      size_t data_len,
                                                      galay_mysql_packet_view_t* out);
GALAY_C_API galay_status_t galay_mysql_encode_query_packet(const char* sql,
                                                           uint8_t sequence_id,
                                                           galay_mysql_buffer_t** out);

GALAY_C_API galay_status_t galay_mysql_client_create(galay_mysql_client_t** out);
GALAY_C_API void galay_mysql_client_destroy(galay_mysql_client_t* client);
GALAY_C_API galay_status_t galay_mysql_client_connect(galay_mysql_client_t* client,
                                                      const galay_mysql_config_t* config);
GALAY_C_API void galay_mysql_client_close(galay_mysql_client_t* client);
GALAY_C_API galay_status_t galay_mysql_client_is_connected(const galay_mysql_client_t* client,
                                                           galay_bool_t* connected);

GALAY_C_END_DECLS

#endif /* GALAY_C_MYSQL_MYSQL_H */
