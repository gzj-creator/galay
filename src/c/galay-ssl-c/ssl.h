#ifndef GALAY_C_SSL_SSL_H
#define GALAY_C_SSL_SSL_H

#include <galay/c/galay-common-c/common/galay_c_error.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum galay_ssl_method_t {
    GALAY_SSL_METHOD_TLS_CLIENT = 0,
    GALAY_SSL_METHOD_TLS_SERVER = 1
} galay_ssl_method_t;

typedef enum galay_ssl_verify_mode_t {
    GALAY_SSL_VERIFY_NONE = 0,
    GALAY_SSL_VERIFY_PEER = 1
} galay_ssl_verify_mode_t;

typedef struct galay_ssl_context_t galay_ssl_context_t;

const char* galay_ssl_get_error(galay_status_t status);
galay_status_t galay_ssl_context_create(galay_ssl_method_t method, galay_ssl_context_t** out);
void galay_ssl_context_destroy(galay_ssl_context_t* context);
galay_status_t galay_ssl_context_load_certificate(galay_ssl_context_t* context, const char* path);
galay_status_t galay_ssl_context_load_private_key(galay_ssl_context_t* context, const char* path);
galay_status_t galay_ssl_context_load_ca(galay_ssl_context_t* context, const char* path);
galay_status_t galay_ssl_context_set_verify_mode(galay_ssl_context_t* context,
                                                 galay_ssl_verify_mode_t mode);

#ifdef __cplusplus
}
#endif

#endif
