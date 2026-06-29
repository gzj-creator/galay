#ifndef GALAY_C_MONGO_MONGO_H
#define GALAY_C_MONGO_MONGO_H

#include <galay/c/galay-common-c/common/galay_c_error.h>
#include <galay/c/galay-kernel-c/coro-c/coro_result_c.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GALAY_MONGO_MAX_KEY_LENGTH 255u
#define GALAY_MONGO_MAX_STRING_LENGTH 4096u

typedef struct galay_mongo_document_t galay_mongo_document_t;
typedef struct galay_mongo_array_t galay_mongo_array_t;
typedef struct galay_mongo_uri_t galay_mongo_uri_t;
typedef struct galay_mongo_client_t galay_mongo_client_t;

const char* galay_mongo_get_error(galay_status_t status);

/**
 * @brief 创建/销毁 BSON 文档；编码结果缓存在文档内，下一次修改或销毁后失效。
 */
galay_status_t galay_mongo_document_create(galay_mongo_document_t** out);
void galay_mongo_document_destroy(galay_mongo_document_t* document);
size_t galay_mongo_document_size(const galay_mongo_document_t* document);
galay_status_t galay_mongo_document_append_int32(galay_mongo_document_t* document, const char* key, int32_t value);
galay_status_t galay_mongo_document_append_int64(galay_mongo_document_t* document, const char* key, int64_t value);
galay_status_t galay_mongo_document_append_double(galay_mongo_document_t* document, const char* key, double value);
galay_status_t galay_mongo_document_append_bool(galay_mongo_document_t* document, const char* key, galay_bool_t value);
galay_status_t galay_mongo_document_append_string(galay_mongo_document_t* document, const char* key, const char* value, size_t value_len);
galay_status_t galay_mongo_document_append_null(galay_mongo_document_t* document, const char* key);
galay_status_t galay_mongo_document_append_document(galay_mongo_document_t* document, const char* key, const galay_mongo_document_t* value);
galay_status_t galay_mongo_document_append_array(galay_mongo_document_t* document, const char* key, const galay_mongo_array_t* value);
galay_status_t galay_mongo_document_append_binary(galay_mongo_document_t* document, const char* key, const uint8_t* value, size_t value_len);
galay_status_t galay_mongo_document_append_object_id(galay_mongo_document_t* document, const char* key, const char* object_id_hex);
galay_status_t galay_mongo_document_append_date_time(galay_mongo_document_t* document, const char* key, int64_t millis);
galay_status_t galay_mongo_document_append_timestamp(galay_mongo_document_t* document, const char* key, uint64_t timestamp);
galay_status_t galay_mongo_document_encode(galay_mongo_document_t* document, const uint8_t** bson, size_t* bson_len);
galay_status_t galay_mongo_document_decode(const uint8_t* bson, size_t bson_len, galay_mongo_document_t** out);
galay_status_t galay_mongo_document_get_int32(const galay_mongo_document_t* document, const char* key, int32_t* value);
galay_status_t galay_mongo_document_get_int64(const galay_mongo_document_t* document, const char* key, int64_t* value);
galay_status_t galay_mongo_document_get_double(const galay_mongo_document_t* document, const char* key, double* value);
galay_status_t galay_mongo_document_get_bool(const galay_mongo_document_t* document, const char* key, galay_bool_t* value);
galay_status_t galay_mongo_document_get_string(const galay_mongo_document_t* document, const char* key, const char** value, size_t* value_len);
galay_status_t galay_mongo_document_get_document(const galay_mongo_document_t* document, const char* key, galay_mongo_document_t** out);
galay_status_t galay_mongo_document_get_array(const galay_mongo_document_t* document, const char* key, galay_mongo_array_t** out);
galay_status_t galay_mongo_document_get_binary(const galay_mongo_document_t* document, const char* key, const uint8_t** value, size_t* value_len);
galay_status_t galay_mongo_document_get_object_id(const galay_mongo_document_t* document, const char* key, const char** value, size_t* value_len);
galay_status_t galay_mongo_document_get_date_time(const galay_mongo_document_t* document, const char* key, int64_t* value);
galay_status_t galay_mongo_document_get_timestamp(const galay_mongo_document_t* document, const char* key, uint64_t* value);
galay_status_t galay_mongo_document_is_null(const galay_mongo_document_t* document, const char* key);

/**
 * @brief BSON 数组；追加到文档或数组时按值复制，调用方仍负责销毁原数组。
 */
galay_status_t galay_mongo_array_create(galay_mongo_array_t** out);
void galay_mongo_array_destroy(galay_mongo_array_t* array);
size_t galay_mongo_array_size(const galay_mongo_array_t* array);
galay_status_t galay_mongo_array_append_int32(galay_mongo_array_t* array, int32_t value);
galay_status_t galay_mongo_array_append_int64(galay_mongo_array_t* array, int64_t value);
galay_status_t galay_mongo_array_append_double(galay_mongo_array_t* array, double value);
galay_status_t galay_mongo_array_append_bool(galay_mongo_array_t* array, galay_bool_t value);
galay_status_t galay_mongo_array_append_string(galay_mongo_array_t* array, const char* value, size_t value_len);
galay_status_t galay_mongo_array_append_null(galay_mongo_array_t* array);
galay_status_t galay_mongo_array_append_document(galay_mongo_array_t* array, const galay_mongo_document_t* value);
galay_status_t galay_mongo_array_append_array(galay_mongo_array_t* array, const galay_mongo_array_t* value);
galay_status_t galay_mongo_array_append_binary(galay_mongo_array_t* array, const uint8_t* value, size_t value_len);
galay_status_t galay_mongo_array_append_object_id(galay_mongo_array_t* array, const char* object_id_hex);
galay_status_t galay_mongo_array_append_date_time(galay_mongo_array_t* array, int64_t millis);
galay_status_t galay_mongo_array_append_timestamp(galay_mongo_array_t* array, uint64_t timestamp);
galay_status_t galay_mongo_array_get_int32(const galay_mongo_array_t* array, size_t index, int32_t* value);
galay_status_t galay_mongo_array_get_int64(const galay_mongo_array_t* array, size_t index, int64_t* value);
galay_status_t galay_mongo_array_get_double(const galay_mongo_array_t* array, size_t index, double* value);
galay_status_t galay_mongo_array_get_bool(const galay_mongo_array_t* array, size_t index, galay_bool_t* value);
galay_status_t galay_mongo_array_get_string(const galay_mongo_array_t* array, size_t index, const char** value, size_t* value_len);
galay_status_t galay_mongo_array_get_document(const galay_mongo_array_t* array, size_t index, galay_mongo_document_t** out);
galay_status_t galay_mongo_array_get_array(const galay_mongo_array_t* array, size_t index, galay_mongo_array_t** out);

galay_status_t galay_mongo_uri_parse(const char* uri_text, galay_mongo_uri_t** out);
void galay_mongo_uri_destroy(galay_mongo_uri_t* uri);
galay_status_t galay_mongo_uri_host(const galay_mongo_uri_t* uri, const char** host, size_t* host_len);
galay_status_t galay_mongo_uri_database(const galay_mongo_uri_t* uri, const char** database, size_t* database_len);
galay_status_t galay_mongo_uri_port(const galay_mongo_uri_t* uri, uint16_t* port);

/**
 * @brief CRUD 命令文档构造器；返回的命令由调用方用 document_destroy 释放。
 */
galay_status_t galay_mongo_command_find_one(const char* database, const char* collection,
                                            const galay_mongo_document_t* filter,
                                            const galay_mongo_document_t* projection,
                                            galay_mongo_document_t** out);
galay_status_t galay_mongo_command_insert_one(const char* database, const char* collection,
                                              const galay_mongo_document_t* document,
                                              galay_mongo_document_t** out);
galay_status_t galay_mongo_command_update_one(const char* database, const char* collection,
                                              const galay_mongo_document_t* filter,
                                              const galay_mongo_document_t* update,
                                              galay_bool_t upsert,
                                              galay_mongo_document_t** out);
galay_status_t galay_mongo_command_delete_one(const char* database, const char* collection,
                                              const galay_mongo_document_t* filter,
                                              galay_mongo_document_t** out);

/**
 * @brief Mongo C coroutine 客户端；async 函数只能在 galay C coroutine 内调用。
 */
galay_status_t galay_mongo_client_create(galay_mongo_client_t** out);
void galay_mongo_client_destroy(galay_mongo_client_t* client);
void galay_mongo_client_close(galay_mongo_client_t* client);
galay_bool_t galay_mongo_client_is_connected(const galay_mongo_client_t* client);
galay_status_t galay_mongo_client_ping(galay_mongo_client_t* client, const char* database);
galay_status_t galay_mongo_client_set_endpoint(galay_mongo_client_t* client, const char* host, uint16_t port, const char* database);
C_IOResult galay_mongo_client_connect_async(galay_mongo_client_t* client, int64_t timeout_ms);
C_IOResult galay_mongo_client_hello_async(galay_mongo_client_t* client, int64_t timeout_ms, galay_mongo_document_t** reply);
C_IOResult galay_mongo_client_command_async(galay_mongo_client_t* client, const char* database,
                                            const galay_mongo_document_t* command,
                                            int64_t timeout_ms,
                                            galay_mongo_document_t** reply);
C_IOResult galay_mongo_client_close_async(galay_mongo_client_t* client, int64_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif
