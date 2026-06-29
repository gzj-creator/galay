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
typedef struct galay_mysql_result_set_t galay_mysql_result_set_t;
typedef struct galay_mysql_stmt_t galay_mysql_stmt_t;
typedef struct galay_mysql_pipeline_t galay_mysql_pipeline_t;
typedef struct galay_mysql_pipeline_result_t galay_mysql_pipeline_result_t;
typedef struct galay_mysql_pool_t galay_mysql_pool_t;
typedef struct galay_mysql_pool_lease_t galay_mysql_pool_lease_t;

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

typedef struct galay_mysql_field_view_t {
    const char* catalog;
    const char* schema;
    const char* table;
    const char* org_table;
    const char* name;
    const char* org_name;
    uint16_t character_set;
    uint32_t column_length;
    uint8_t column_type;
    uint16_t flags;
    uint8_t decimals;
} galay_mysql_field_view_t;

typedef struct galay_mysql_value_view_t {
    const unsigned char* data;
    size_t data_len;
    galay_bool_t is_null;
} galay_mysql_value_view_t;

typedef struct galay_mysql_stmt_bind_t {
    const unsigned char* data;
    size_t data_len;
    galay_bool_t is_null;
    uint8_t column_type;
} galay_mysql_stmt_bind_t;

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

/**
 * @brief 解码一段连续 MySQL response packet 为 C result-set 对象。
 * @param data 连续完整 packet buffer，每个 packet 包含 4 字节 MySQL packet header。
 * @param data_len buffer 字节数。
 * @param out 成功时获得 result-set 所有权，调用方必须用 `galay_mysql_result_set_destroy` 释放。
 * @return `GALAY_OK` 表示解码成功；截断、非法长度或不支持的响应返回 `GALAY_PROTOCOL_ERROR`。
 * @note 返回的 field/value view 均借用 result-set 内部存储，仅在 result-set 销毁前有效。
 */
galay_status_t galay_mysql_result_set_decode(const unsigned char* data, size_t data_len,
                                             galay_mysql_result_set_t** out);
void galay_mysql_result_set_destroy(galay_mysql_result_set_t* result);
galay_status_t galay_mysql_result_set_field_count(const galay_mysql_result_set_t* result,
                                                  size_t* count);
galay_status_t galay_mysql_result_set_row_count(const galay_mysql_result_set_t* result,
                                                size_t* count);
galay_status_t galay_mysql_result_set_field(const galay_mysql_result_set_t* result,
                                            size_t index,
                                            galay_mysql_field_view_t* field);
galay_status_t galay_mysql_result_set_find_field(const galay_mysql_result_set_t* result,
                                                 const char* name,
                                                 size_t* index);
galay_status_t galay_mysql_result_set_value(const galay_mysql_result_set_t* result,
                                            size_t row,
                                            size_t column,
                                            galay_mysql_value_view_t* value);
galay_status_t galay_mysql_result_set_affected_rows(const galay_mysql_result_set_t* result,
                                                    uint64_t* affected_rows);
galay_status_t galay_mysql_result_set_last_insert_id(const galay_mysql_result_set_t* result,
                                                     uint64_t* last_insert_id);
galay_status_t galay_mysql_result_set_status_flags(const galay_mysql_result_set_t* result,
                                                   uint16_t* status_flags);
galay_status_t galay_mysql_result_set_warnings(const galay_mysql_result_set_t* result,
                                               uint16_t* warnings);

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
 * @brief 使用最近一次 handshake packet 发送 MySQL 认证响应并读取认证结果。
 * @param client 已通过 `galay_mysql_client_connect_async` 建立 TCP 并读取 handshake 的 client。
 * @param config 用户名、密码、database 等认证配置。
 * @param timeout_ms socket I/O 超时。
 * @return `C_IOResultOk` 表示认证 OK packet 已收到；不支持的插件、ERR packet 或协议错误返回错误。
 * @note C ABI 支持 `mysql_native_password` 和 `caching_sha2_password`，后者覆盖 fast auth
 *       与 RSA public-key full auth；full auth 需要构建时启用 SSL/RSA 支持。
 */
C_IOResult galay_mysql_client_authenticate_async(galay_mysql_client_t* client,
                                                 const galay_mysql_config_t* config,
                                                 int64_t timeout_ms);

C_IOResult galay_mysql_client_connect_auth_async(galay_mysql_client_t* client,
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

C_IOResult galay_mysql_client_query_result_async(galay_mysql_client_t* client,
                                                 const char* query,
                                                 int64_t timeout_ms,
                                                 galay_mysql_result_set_t** result);
C_IOResult galay_mysql_client_begin_transaction_async(galay_mysql_client_t* client,
                                                      int64_t timeout_ms,
                                                      galay_mysql_result_set_t** result);
C_IOResult galay_mysql_client_commit_async(galay_mysql_client_t* client,
                                           int64_t timeout_ms,
                                           galay_mysql_result_set_t** result);
C_IOResult galay_mysql_client_rollback_async(galay_mysql_client_t* client,
                                             int64_t timeout_ms,
                                             galay_mysql_result_set_t** result);
C_IOResult galay_mysql_client_stmt_prepare_async(galay_mysql_client_t* client,
                                                 const char* sql,
                                                 int64_t timeout_ms,
                                                 galay_mysql_stmt_t** stmt);
void galay_mysql_stmt_destroy(galay_mysql_stmt_t* stmt);
galay_status_t galay_mysql_stmt_id(const galay_mysql_stmt_t* stmt, uint32_t* statement_id);
galay_status_t galay_mysql_stmt_param_count(const galay_mysql_stmt_t* stmt, size_t* count);
galay_status_t galay_mysql_stmt_column_count(const galay_mysql_stmt_t* stmt, size_t* count);
C_IOResult galay_mysql_client_stmt_execute_async(galay_mysql_client_t* client,
                                                 const galay_mysql_stmt_t* stmt,
                                                 const galay_mysql_stmt_bind_t* binds,
                                                 size_t bind_count,
                                                 int64_t timeout_ms,
                                                 galay_mysql_result_set_t** result);
galay_status_t galay_mysql_pipeline_create(galay_mysql_pipeline_t** out);
void galay_mysql_pipeline_destroy(galay_mysql_pipeline_t* pipeline);
galay_status_t galay_mysql_pipeline_append_query(galay_mysql_pipeline_t* pipeline,
                                                 const char* query);
C_IOResult galay_mysql_client_pipeline_async(galay_mysql_client_t* client,
                                             const galay_mysql_pipeline_t* pipeline,
                                             int64_t timeout_ms,
                                             galay_mysql_pipeline_result_t** result);
void galay_mysql_pipeline_result_destroy(galay_mysql_pipeline_result_t* result);
galay_status_t galay_mysql_pipeline_result_count(const galay_mysql_pipeline_result_t* result,
                                                 size_t* count);
galay_status_t galay_mysql_pipeline_result_at(const galay_mysql_pipeline_result_t* result,
                                              size_t index,
                                              const galay_mysql_result_set_t** item);

galay_status_t galay_mysql_pool_create(const galay_mysql_config_t* config,
                                       size_t max_connections,
                                       galay_mysql_pool_t** out);
void galay_mysql_pool_destroy(galay_mysql_pool_t* pool);
C_IOResult galay_mysql_pool_acquire_async(galay_mysql_pool_t* pool,
                                          int64_t timeout_ms,
                                          galay_mysql_pool_lease_t** lease);
galay_status_t galay_mysql_pool_lease_client(galay_mysql_pool_lease_t* lease,
                                             galay_mysql_client_t** client);
galay_status_t galay_mysql_pool_lease_release(galay_mysql_pool_lease_t* lease);

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
