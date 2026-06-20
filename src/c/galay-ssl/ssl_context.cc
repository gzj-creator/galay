#include <galay/c/galay-ssl/ssl.h>

#include <galay/cpp/galay-ssl/ssl/ssl_context.h>

#include <filesystem>
#include <new>
#include <utility>

struct galay_ssl_context {
    galay::ssl::SslContext impl;
};

namespace {

bool to_cpp_method(galay_ssl_method_t method, galay::ssl::SslMethod& out)
{
    switch (method) {
    case GALAY_SSL_METHOD_TLS_CLIENT:
        out = galay::ssl::SslMethod::TLS_Client;
        return true;
    case GALAY_SSL_METHOD_TLS_SERVER:
        out = galay::ssl::SslMethod::TLS_Server;
        return true;
    case GALAY_SSL_METHOD_TLS_1_2_CLIENT:
        out = galay::ssl::SslMethod::TLS_1_2_Client;
        return true;
    case GALAY_SSL_METHOD_TLS_1_2_SERVER:
        out = galay::ssl::SslMethod::TLS_1_2_Server;
        return true;
    case GALAY_SSL_METHOD_TLS_1_3_CLIENT:
        out = galay::ssl::SslMethod::TLS_1_3_Client;
        return true;
    case GALAY_SSL_METHOD_TLS_1_3_SERVER:
        out = galay::ssl::SslMethod::TLS_1_3_Server;
        return true;
    case GALAY_SSL_METHOD_DTLS_CLIENT:
        out = galay::ssl::SslMethod::DTLS_Client;
        return true;
    case GALAY_SSL_METHOD_DTLS_SERVER:
        out = galay::ssl::SslMethod::DTLS_Server;
        return true;
    default:
        return false;
    }
}

bool to_cpp_verify_mode(galay_ssl_verify_mode_t mode, galay::ssl::SslVerifyMode& out)
{
    switch (mode) {
    case GALAY_SSL_VERIFY_NONE:
        out = galay::ssl::SslVerifyMode::None;
        return true;
    case GALAY_SSL_VERIFY_PEER:
        out = galay::ssl::SslVerifyMode::Peer;
        return true;
    case GALAY_SSL_VERIFY_FAIL_IF_NO_PEER_CERT:
        out = galay::ssl::SslVerifyMode::FailIfNoPeerCert;
        return true;
    case GALAY_SSL_VERIFY_CLIENT_ONCE:
        out = galay::ssl::SslVerifyMode::ClientOnce;
        return true;
    default:
        return false;
    }
}

galay_status_t validate_context_and_path(galay_ssl_context_t* context, const char* path)
{
    if (context == nullptr || path == nullptr || path[0] == '\0') {
        return GALAY_INVALID_ARGUMENT;
    }

    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) {
        return GALAY_NOT_FOUND;
    }
    return GALAY_OK;
}

template <typename LoadFn>
galay_status_t load_file(galay_ssl_context_t* context, const char* path, LoadFn load)
{
    try {
        const galay_status_t validation = validate_context_and_path(context, path);
        if (validation != GALAY_OK) {
            return validation;
        }

        auto result = load(context->impl, path);
        return result ? GALAY_OK : GALAY_IO_ERROR;
    } catch (const std::bad_alloc&) {
        return GALAY_OUT_OF_MEMORY;
    } catch (...) {
        return GALAY_INTERNAL_ERROR;
    }
}

} // namespace

galay_status_t galay_ssl_context_create(
    galay_ssl_method_t method,
    galay_ssl_context_t** out_context)
{
    if (out_context == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    *out_context = nullptr;

    galay::ssl::SslMethod cpp_method{};
    if (!to_cpp_method(method, cpp_method)) {
        return GALAY_INVALID_ARGUMENT;
    }

    try {
        auto* context = new (std::nothrow) galay_ssl_context{galay::ssl::SslContext(cpp_method)};
        if (context == nullptr) {
            return GALAY_OUT_OF_MEMORY;
        }
        if (!context->impl.isValid()) {
            delete context;
            return GALAY_INTERNAL_ERROR;
        }

        *out_context = context;
        return GALAY_OK;
    } catch (const std::bad_alloc&) {
        return GALAY_OUT_OF_MEMORY;
    } catch (...) {
        return GALAY_INTERNAL_ERROR;
    }
}

void galay_ssl_context_destroy(galay_ssl_context_t* context)
{
    delete context;
}

galay_status_t galay_ssl_context_load_certificate(
    galay_ssl_context_t* context,
    const char* cert_file)
{
    return load_file(context, cert_file, [](galay::ssl::SslContext& impl, const char* path) {
        return impl.loadCertificate(path);
    });
}

galay_status_t galay_ssl_context_load_private_key(
    galay_ssl_context_t* context,
    const char* key_file)
{
    return load_file(context, key_file, [](galay::ssl::SslContext& impl, const char* path) {
        return impl.loadPrivateKey(path);
    });
}

galay_status_t galay_ssl_context_load_ca(
    galay_ssl_context_t* context,
    const char* ca_file)
{
    return load_file(context, ca_file, [](galay::ssl::SslContext& impl, const char* path) {
        return impl.loadCACertificate(path);
    });
}

galay_status_t galay_ssl_context_set_verify_mode(
    galay_ssl_context_t* context,
    galay_ssl_verify_mode_t mode)
{
    if (context == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }

    galay::ssl::SslVerifyMode cpp_mode{};
    if (!to_cpp_verify_mode(mode, cpp_mode)) {
        return GALAY_INVALID_ARGUMENT;
    }

    try {
        context->impl.setVerifyMode(cpp_mode);
        return GALAY_OK;
    } catch (const std::bad_alloc&) {
        return GALAY_OUT_OF_MEMORY;
    } catch (...) {
        return GALAY_INTERNAL_ERROR;
    }
}
