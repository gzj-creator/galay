/**
 * @file http.h
 * @brief galay-http C ABI 封装。
 *
 * @details 该头文件只暴露 C 兼容的 opaque handle、枚举和显式错误码。
 *          返回的 const char* 指针均由对应 handle 持有，在下一次修改该 handle
 *          或 destroy 前有效，调用方不得释放。
 */

#ifndef GALAY_C_HTTP_HTTP_H
#define GALAY_C_HTTP_HTTP_H

#include <galay/c/galay-c/common/galay_c_error.h>

GALAY_C_BEGIN_DECLS

typedef struct galay_http_headers galay_http_headers_t;
typedef struct galay_http_request galay_http_request_t;
typedef struct galay_http_response galay_http_response_t;

typedef enum galay_http_method {
    GALAY_HTTP_METHOD_GET = 0,
    GALAY_HTTP_METHOD_POST = 1,
    GALAY_HTTP_METHOD_HEAD = 2,
    GALAY_HTTP_METHOD_PUT = 3,
    GALAY_HTTP_METHOD_DELETE = 4,
    GALAY_HTTP_METHOD_TRACE = 5,
    GALAY_HTTP_METHOD_OPTIONS = 6,
    GALAY_HTTP_METHOD_CONNECT = 7,
    GALAY_HTTP_METHOD_PATCH = 8,
    GALAY_HTTP_METHOD_PRI = 9,
    GALAY_HTTP_METHOD_UNKNOWN = 10
} galay_http_method_t;

typedef enum galay_http_version {
    GALAY_HTTP_VERSION_1_0 = 0,
    GALAY_HTTP_VERSION_1_1 = 1,
    GALAY_HTTP_VERSION_2_0 = 2,
    GALAY_HTTP_VERSION_3_0 = 3,
    GALAY_HTTP_VERSION_UNKNOWN = 4
} galay_http_version_t;

typedef enum galay_http_status_code {
    GALAY_HTTP_STATUS_OK = 200,
    GALAY_HTTP_STATUS_CREATED = 201,
    GALAY_HTTP_STATUS_NO_CONTENT = 204,
    GALAY_HTTP_STATUS_BAD_REQUEST = 400,
    GALAY_HTTP_STATUS_NOT_FOUND = 404,
    GALAY_HTTP_STATUS_PAYLOAD_TOO_LARGE = 413,
    GALAY_HTTP_STATUS_INTERNAL_SERVER_ERROR = 500
} galay_http_status_code_t;

GALAY_C_API galay_status_t galay_http_headers_create(galay_http_headers_t** out);
GALAY_C_API void galay_http_headers_destroy(galay_http_headers_t* headers);
GALAY_C_API galay_status_t galay_http_headers_add(galay_http_headers_t* headers,
                                                  const char* key,
                                                  const char* value);
GALAY_C_API galay_status_t galay_http_headers_find(const galay_http_headers_t* headers,
                                                   const char* key,
                                                   const char** value,
                                                   size_t* value_len);
GALAY_C_API galay_status_t galay_http_headers_remove(galay_http_headers_t* headers,
                                                     const char* key);

GALAY_C_API galay_status_t galay_http_request_create(galay_http_request_t** out);
GALAY_C_API void galay_http_request_destroy(galay_http_request_t* request);
GALAY_C_API void galay_http_request_reset(galay_http_request_t* request);
GALAY_C_API galay_status_t galay_http_request_set_method(galay_http_request_t* request,
                                                         galay_http_method_t method);
GALAY_C_API galay_status_t galay_http_request_method(const galay_http_request_t* request,
                                                     galay_http_method_t* method);
GALAY_C_API galay_status_t galay_http_request_set_uri(galay_http_request_t* request,
                                                      const char* uri);
GALAY_C_API galay_status_t galay_http_request_set_version(galay_http_request_t* request,
                                                          galay_http_version_t version);
GALAY_C_API galay_status_t galay_http_request_add_header(galay_http_request_t* request,
                                                         const char* key,
                                                         const char* value);
GALAY_C_API galay_status_t galay_http_request_find_header(const galay_http_request_t* request,
                                                          const char* key,
                                                          const char** value,
                                                          size_t* value_len);
GALAY_C_API galay_status_t galay_http_request_remove_header(galay_http_request_t* request,
                                                            const char* key);
GALAY_C_API galay_status_t galay_http_request_set_body(galay_http_request_t* request,
                                                       const void* data,
                                                       size_t data_len);
GALAY_C_API galay_status_t galay_http_request_body(const galay_http_request_t* request,
                                                   const char** data,
                                                   size_t* data_len);
GALAY_C_API galay_status_t galay_http_request_parse(galay_http_request_t* request,
                                                    const void* data,
                                                    size_t data_len,
                                                    size_t max_header_size,
                                                    size_t max_body_size,
                                                    size_t* consumed);
GALAY_C_API galay_bool_t galay_http_request_is_complete(const galay_http_request_t* request);
GALAY_C_API galay_status_t galay_http_request_serialize(galay_http_request_t* request,
                                                        const char** data,
                                                        size_t* data_len);

GALAY_C_API galay_status_t galay_http_response_create(galay_http_response_t** out);
GALAY_C_API void galay_http_response_destroy(galay_http_response_t* response);
GALAY_C_API void galay_http_response_reset(galay_http_response_t* response);
GALAY_C_API galay_status_t galay_http_response_set_status(galay_http_response_t* response,
                                                          galay_http_status_code_t status);
GALAY_C_API galay_status_t galay_http_response_status(const galay_http_response_t* response,
                                                      galay_http_status_code_t* status);
GALAY_C_API galay_status_t galay_http_response_set_version(galay_http_response_t* response,
                                                           galay_http_version_t version);
GALAY_C_API galay_status_t galay_http_response_add_header(galay_http_response_t* response,
                                                          const char* key,
                                                          const char* value);
GALAY_C_API galay_status_t galay_http_response_find_header(const galay_http_response_t* response,
                                                           const char* key,
                                                           const char** value,
                                                           size_t* value_len);
GALAY_C_API galay_status_t galay_http_response_remove_header(galay_http_response_t* response,
                                                             const char* key);
GALAY_C_API galay_status_t galay_http_response_set_body(galay_http_response_t* response,
                                                        const void* data,
                                                        size_t data_len);
GALAY_C_API galay_status_t galay_http_response_body(const galay_http_response_t* response,
                                                    const char** data,
                                                    size_t* data_len);
GALAY_C_API galay_status_t galay_http_response_parse(galay_http_response_t* response,
                                                     const void* data,
                                                     size_t data_len,
                                                     size_t max_header_size,
                                                     size_t max_body_size,
                                                     size_t* consumed);
GALAY_C_API galay_bool_t galay_http_response_is_complete(const galay_http_response_t* response);
GALAY_C_API galay_status_t galay_http_response_serialize(galay_http_response_t* response,
                                                         const char** data,
                                                         size_t* data_len);

GALAY_C_END_DECLS

#endif /* GALAY_C_HTTP_HTTP_H */
