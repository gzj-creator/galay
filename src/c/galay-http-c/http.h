#ifndef GALAY_C_HTTP_HTTP_H
#define GALAY_C_HTTP_HTTP_H

#include <galay/c/galay-common-c/common/galay_c_error.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum galay_http_method_t {
    GALAY_HTTP_METHOD_GET = 0,
    GALAY_HTTP_METHOD_POST = 1
} galay_http_method_t;

typedef enum galay_http_status_code_t {
    GALAY_HTTP_STATUS_OK = 200,
    GALAY_HTTP_STATUS_NO_CONTENT = 204
} galay_http_status_code_t;

typedef struct galay_http_headers_t galay_http_headers_t;
typedef struct galay_http_request_t galay_http_request_t;
typedef struct galay_http_response_t galay_http_response_t;

const char* galay_http_get_error(galay_status_t status);
galay_status_t galay_http_headers_create(galay_http_headers_t** out);
void galay_http_headers_destroy(galay_http_headers_t* headers);
galay_status_t galay_http_headers_add(galay_http_headers_t* headers, const char* name,
                                      const char* value);
galay_status_t galay_http_headers_find(const galay_http_headers_t* headers, const char* name,
                                       const char** value, size_t* value_len);
galay_status_t galay_http_headers_remove(galay_http_headers_t* headers, const char* name);

galay_status_t galay_http_request_create(galay_http_request_t** out);
void galay_http_request_destroy(galay_http_request_t* request);
galay_status_t galay_http_request_parse(galay_http_request_t* request, const char* data,
                                        size_t data_len, size_t max_header_len,
                                        size_t max_body_len, size_t* consumed);
galay_bool_t galay_http_request_is_complete(const galay_http_request_t* request);
galay_status_t galay_http_request_body(const galay_http_request_t* request, const char** body,
                                       size_t* body_len);
galay_status_t galay_http_request_find_header(const galay_http_request_t* request,
                                              const char* name, const char** value,
                                              size_t* value_len);

galay_status_t galay_http_response_create(galay_http_response_t** out);
void galay_http_response_destroy(galay_http_response_t* response);
galay_status_t galay_http_response_set_status(galay_http_response_t* response,
                                              galay_http_status_code_t status);
galay_status_t galay_http_response_set_body(galay_http_response_t* response, const char* body,
                                            size_t body_len);
galay_status_t galay_http_response_serialize(galay_http_response_t* response,
                                             const char** data, size_t* data_len);

#ifdef __cplusplus
}
#endif

#endif
