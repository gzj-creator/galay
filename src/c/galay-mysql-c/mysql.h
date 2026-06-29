#ifndef GALAY_C_MYSQL_MYSQL_H
#define GALAY_C_MYSQL_MYSQL_H

#include <galay/c/galay-common-c/common/galay_c_error.h>
#include <galay/c/galay-kernel-c/coro-c/coro_result_c.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct galay_mysql_config_t galay_mysql_config_t;
typedef struct galay_mysql_buffer_t galay_mysql_buffer_t;
typedef struct galay_mysql_client_t galay_mysql_client_t;

typedef struct galay_mysql_packet_header_t {
    uint32_t payload_length;
    uint8_t sequence_id;
} galay_mysql_packet_header_t;

typedef struct galay_mysql_packet_view_t {
    const unsigned char* payload;
    size_t payload_len;
    uint8_t sequence_id;
    size_t consumed;
} galay_mysql_packet_view_t;

const char* galay_mysql_get_error(galay_status_t status);
galay_status_t galay_mysql_config_create(galay_mysql_config_t** out);
void galay_mysql_config_destroy(galay_mysql_config_t* config);
galay_status_t galay_mysql_config_host(const galay_mysql_config_t* config, const char** host);
galay_status_t galay_mysql_config_port(const galay_mysql_config_t* config, uint16_t* port);
galay_status_t galay_mysql_config_set_host(galay_mysql_config_t* config, const char* host);
galay_status_t galay_mysql_config_set_port(galay_mysql_config_t* config, uint16_t port);
galay_status_t galay_mysql_config_set_username(galay_mysql_config_t* config, const char* username);
galay_status_t galay_mysql_config_set_password(galay_mysql_config_t* config, const char* password);
galay_status_t galay_mysql_config_set_database(galay_mysql_config_t* config, const char* database);
galay_status_t galay_mysql_config_set_charset(galay_mysql_config_t* config, const char* charset);
galay_status_t galay_mysql_config_set_connect_timeout_ms(galay_mysql_config_t* config, uint32_t timeout_ms);
galay_status_t galay_mysql_config_validate(const galay_mysql_config_t* config);
galay_status_t galay_mysql_auth_response_for_plugin(const char* plugin, const char* password,
                                                    const unsigned char* salt, size_t salt_len,
                                                    galay_mysql_buffer_t** out);
void galay_mysql_buffer_destroy(galay_mysql_buffer_t* buffer);
galay_status_t galay_mysql_buffer_data(const galay_mysql_buffer_t* buffer,
                                       const unsigned char** data, size_t* data_len);
galay_status_t galay_mysql_parse_packet_header(const unsigned char* data, size_t data_len,
                                               galay_mysql_packet_header_t* header);
galay_status_t galay_mysql_extract_packet(const unsigned char* data, size_t data_len,
                                          galay_mysql_packet_view_t* view);
galay_status_t galay_mysql_encode_query_packet(const char* query, uint8_t sequence_id,
                                               galay_mysql_buffer_t** out);
galay_status_t galay_mysql_client_create(galay_mysql_client_t** out);
void galay_mysql_client_destroy(galay_mysql_client_t* client);
void galay_mysql_client_close(galay_mysql_client_t* client);
galay_status_t galay_mysql_client_is_connected(const galay_mysql_client_t* client,
                                               galay_bool_t* connected);
galay_status_t galay_mysql_client_connect(galay_mysql_client_t* client,
                                          const galay_mysql_config_t* config);

/**
 * @brief 在当前 C coroutine 内异步连接 MySQL mock/standalone endpoint 并读取 server handshake packet。
 * @param client 由 `galay_mysql_client_create` 创建的 client。
 * @param config 连接配置；host/port 必须有效。
 * @param timeout_ms 负数时使用 config 的 connect timeout，0 直接超时，正数为毫秒超时。
 * @return `C_IOResultOk` 表示 TCP 连接成功且读取到一包 handshake；错误通过返回值传播。
 * @note 当前 C async 最小闭环只读取 handshake packet，不实现完整 MySQL auth exchange。
 */
C_IOResult galay_mysql_client_connect_async(galay_mysql_client_t* client,
                                            const galay_mysql_config_t* config,
                                            int64_t timeout_ms);

/**
 * @brief 在当前 C coroutine 内发送 COM_QUERY 并读取一个 result/error packet。
 * @param client 已通过 `galay_mysql_client_connect_async` 连接的 client。
 * @param query SQL 文本，不能为 NULL。
 * @param timeout_ms 每次 socket I/O 的毫秒超时。
 * @param result_packet 成功时返回完整 MySQL packet buffer，调用方用
 *        `galay_mysql_buffer_destroy` 释放。
 * @return `C_IOResultOk` 表示 query packet 写入且读取到一包结果；错误通过返回值传播。
 */
C_IOResult galay_mysql_client_query_async(galay_mysql_client_t* client,
                                          const char* query,
                                          int64_t timeout_ms,
                                          galay_mysql_buffer_t** result_packet);

/**
 * @brief 在当前 C coroutine 内关闭 MySQL TCP 连接并释放内部 socket。
 * @param client 已连接或持有 socket 的 client。
 * @param timeout_ms 关闭操作超时，语义同 kernel TCP close。
 * @return `C_IOResultOk` 表示关闭并清理成功；失败通过返回值传播。
 */
C_IOResult galay_mysql_client_close_async(galay_mysql_client_t* client, int64_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif
