/**
 * @file mongo.h
 * @brief galay-mongo C ABI 封装。
 *
 * @details 该头文件只暴露 C 兼容的 opaque handle、稳定错误码和显式
 *          create/destroy 生命周期。返回的 data 指针均由对应 handle 持有，
 *          在下一次修改该 handle 或 destroy 前有效，调用方不得释放。
 */

#ifndef GALAY_C_MONGO_MONGO_H
#define GALAY_C_MONGO_MONGO_H

#include <galay/c/galay-c/common/galay_c_error.h>

GALAY_C_BEGIN_DECLS

enum {
    GALAY_MONGO_MAX_KEY_LENGTH = 1024,
    GALAY_MONGO_MAX_STRING_LENGTH = 4096
};

typedef struct galay_mongo_document galay_mongo_document_t;
typedef struct galay_mongo_uri galay_mongo_uri_t;
typedef struct galay_mongo_client galay_mongo_client_t;

GALAY_C_API galay_status_t galay_mongo_document_create(galay_mongo_document_t** out);
GALAY_C_API void galay_mongo_document_destroy(galay_mongo_document_t* document);
GALAY_C_API size_t galay_mongo_document_size(const galay_mongo_document_t* document);

GALAY_C_API galay_status_t galay_mongo_document_append_int32(galay_mongo_document_t* document,
                                                             const char* key,
                                                             int32_t value);
GALAY_C_API galay_status_t galay_mongo_document_append_int64(galay_mongo_document_t* document,
                                                             const char* key,
                                                             int64_t value);
GALAY_C_API galay_status_t galay_mongo_document_append_double(galay_mongo_document_t* document,
                                                              const char* key,
                                                              double value);
GALAY_C_API galay_status_t galay_mongo_document_append_bool(galay_mongo_document_t* document,
                                                            const char* key,
                                                            galay_bool_t value);
GALAY_C_API galay_status_t galay_mongo_document_append_string(galay_mongo_document_t* document,
                                                              const char* key,
                                                              const char* value,
                                                              size_t value_len);
GALAY_C_API galay_status_t galay_mongo_document_append_null(galay_mongo_document_t* document,
                                                            const char* key);

GALAY_C_API galay_status_t galay_mongo_document_get_int32(const galay_mongo_document_t* document,
                                                          const char* key,
                                                          int32_t* value);
GALAY_C_API galay_status_t galay_mongo_document_get_int64(const galay_mongo_document_t* document,
                                                          const char* key,
                                                          int64_t* value);
GALAY_C_API galay_status_t galay_mongo_document_get_double(const galay_mongo_document_t* document,
                                                           const char* key,
                                                           double* value);
GALAY_C_API galay_status_t galay_mongo_document_get_bool(const galay_mongo_document_t* document,
                                                         const char* key,
                                                         galay_bool_t* value);
GALAY_C_API galay_status_t galay_mongo_document_get_string(const galay_mongo_document_t* document,
                                                           const char* key,
                                                           const char** value,
                                                           size_t* value_len);

GALAY_C_API galay_status_t galay_mongo_document_encode(galay_mongo_document_t* document,
                                                       const uint8_t** data,
                                                       size_t* data_len);
GALAY_C_API galay_status_t galay_mongo_document_decode(const uint8_t* data,
                                                       size_t data_len,
                                                       galay_mongo_document_t** out);

GALAY_C_API galay_status_t galay_mongo_uri_parse(const char* uri, galay_mongo_uri_t** out);
GALAY_C_API void galay_mongo_uri_destroy(galay_mongo_uri_t* uri);
GALAY_C_API galay_status_t galay_mongo_uri_host(const galay_mongo_uri_t* uri,
                                                const char** value,
                                                size_t* value_len);
GALAY_C_API galay_status_t galay_mongo_uri_database(const galay_mongo_uri_t* uri,
                                                    const char** value,
                                                    size_t* value_len);
GALAY_C_API galay_status_t galay_mongo_uri_port(const galay_mongo_uri_t* uri,
                                                uint16_t* port);

GALAY_C_API galay_status_t galay_mongo_client_create(galay_mongo_client_t** out);
GALAY_C_API void galay_mongo_client_destroy(galay_mongo_client_t* client);
GALAY_C_API galay_status_t galay_mongo_client_connect_uri(galay_mongo_client_t* client,
                                                          const char* uri);
GALAY_C_API galay_status_t galay_mongo_client_ping(galay_mongo_client_t* client,
                                                   const char* database);
GALAY_C_API void galay_mongo_client_close(galay_mongo_client_t* client);
GALAY_C_API galay_bool_t galay_mongo_client_is_connected(const galay_mongo_client_t* client);

GALAY_C_END_DECLS

#endif /* GALAY_C_MONGO_MONGO_H */
