#include <galay/c/galay-ssl-c/ssl.h>

#include <cstdio>
#include <new>

struct galay_ssl_context_t {
    galay_ssl_method_t method = GALAY_SSL_METHOD_TLS_CLIENT;
    galay_ssl_verify_mode_t verify_mode = GALAY_SSL_VERIFY_NONE;
};

static galay_status_t file_exists(const char* path)
{
    if (path == nullptr || path[0] == '\0') {
        return GALAY_INVALID_ARGUMENT;
    }
    FILE* file = std::fopen(path, "rb");
    if (file == nullptr) {
        return GALAY_NOT_FOUND;
    }
    const int close_result = std::fclose(file);
    if (close_result != 0) {
        return GALAY_IO_ERROR;
    }
    return GALAY_OK;
}

extern "C" {

const char* galay_ssl_get_error(galay_status_t status)
{
    return galay_status_string(status);
}

galay_status_t galay_ssl_context_create(galay_ssl_method_t method, galay_ssl_context_t** out)
{
    if (out == nullptr || (method != GALAY_SSL_METHOD_TLS_CLIENT && method != GALAY_SSL_METHOD_TLS_SERVER)) {
        return GALAY_INVALID_ARGUMENT;
    }
    auto* context = new (std::nothrow) galay_ssl_context_t();
    if (context == nullptr) {
        *out = nullptr;
        return GALAY_OUT_OF_MEMORY;
    }
    context->method = method;
    *out = context;
    return GALAY_OK;
}

void galay_ssl_context_destroy(galay_ssl_context_t* context)
{
    delete context;
}

galay_status_t galay_ssl_context_load_certificate(galay_ssl_context_t* context, const char* path)
{
    if (context == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    return file_exists(path);
}

galay_status_t galay_ssl_context_load_private_key(galay_ssl_context_t* context, const char* path)
{
    if (context == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    return file_exists(path);
}

galay_status_t galay_ssl_context_load_ca(galay_ssl_context_t* context, const char* path)
{
    if (context == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    return file_exists(path);
}

galay_status_t galay_ssl_context_set_verify_mode(galay_ssl_context_t* context,
                                                 galay_ssl_verify_mode_t mode)
{
    if (context == nullptr || (mode != GALAY_SSL_VERIFY_NONE && mode != GALAY_SSL_VERIFY_PEER)) {
        return GALAY_INVALID_ARGUMENT;
    }
    context->verify_mode = mode;
    return GALAY_OK;
}

}
