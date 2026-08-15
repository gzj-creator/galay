#include <galay/c/galay-ssl-c/ssl.h>

#include <openssl/err.h>
#include <openssl/ssl.h>

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { kCipherBufferSize = 16 * 1024, kMaxSslSteps = 4096 };

struct galay_ssl_context_t {
    SSL_CTX* context;
    unsigned char* server_alpn;
    unsigned int server_alpn_len;
    galay_ssl_method_t method;
    galay_ssl_verify_mode_t verify_mode;
};

struct galay_ssl_socket_t {
    galay_ssl_context_t* context;
    SSL* engine;
    galay_c_tcp_socket_t transport;
    int handshake_complete;
};

static C_IOResult make_io_result(C_IOResultCode code, int sys_errno, size_t bytes)
{
    C_IOResult result = {code, sys_errno, bytes, 0, NULL};
    return result;
}

static int valid_ip_type(C_IPType type)
{
    return type == C_IPTypeIPV4 || type == C_IPTypeIPV6;
}

static int valid_context(const galay_ssl_context_t* context)
{
    return context != NULL && context->context != NULL;
}

static int valid_socket(const galay_ssl_socket_t* socket)
{
    return socket != NULL && valid_context(socket->context) && socket->engine != NULL &&
        socket->transport.fd >= 0;
}

static galay_status_t file_exists(const char* path)
{
    if (path == NULL || path[0] == '\0') {
        return GALAY_INVALID_ARGUMENT;
    }
    FILE* file = fopen(path, "rb");
    if (file == NULL) {
        return errno == ENOENT ? GALAY_NOT_FOUND : GALAY_IO_ERROR;
    }
    return fclose(file) == 0 ? GALAY_OK : GALAY_IO_ERROR;
}

static int session_cache_mode(galay_ssl_session_cache_mode_t mode, long* value)
{
    if (value == NULL) {
        return 0;
    }
    switch (mode) {
    case GALAY_SSL_SESSION_CACHE_OFF:
        *value = SSL_SESS_CACHE_OFF;
        return 1;
    case GALAY_SSL_SESSION_CACHE_CLIENT:
        *value = SSL_SESS_CACHE_CLIENT;
        return 1;
    case GALAY_SSL_SESSION_CACHE_SERVER:
        *value = SSL_SESS_CACHE_SERVER;
        return 1;
    case GALAY_SSL_SESSION_CACHE_BOTH:
        *value = SSL_SESS_CACHE_BOTH;
        return 1;
    }
    return 0;
}

static galay_status_t make_alpn_wire(const char* const* protocols,
                                     size_t count,
                                     unsigned char** wire,
                                     unsigned int* wire_len)
{
    if (protocols == NULL || count == 0 || wire == NULL || wire_len == NULL) {
        return GALAY_INVALID_ARGUMENT;
    }
    *wire = NULL;
    *wire_len = 0;
    size_t total = 0;
    for (size_t index = 0; index < count; ++index) {
        if (protocols[index] == NULL) {
            return GALAY_INVALID_ARGUMENT;
        }
        const size_t length = strlen(protocols[index]);
        if (length == 0 || length > 255 || total > UINT_MAX - length - 1U) {
            return GALAY_INVALID_ARGUMENT;
        }
        total += length + 1U;
    }
    unsigned char* encoded = malloc(total);
    if (encoded == NULL) {
        return GALAY_OUT_OF_MEMORY;
    }
    size_t position = 0;
    for (size_t index = 0; index < count; ++index) {
        const size_t length = strlen(protocols[index]);
        encoded[position++] = (unsigned char)length;
        memcpy(encoded + position, protocols[index], length);
        position += length;
    }
    *wire = encoded;
    *wire_len = (unsigned int)total;
    return GALAY_OK;
}

static int select_alpn(SSL* ssl,
                       const unsigned char** out,
                       unsigned char* out_len,
                       const unsigned char* in,
                       unsigned int in_len,
                       void* argument)
{
    (void)ssl;
    galay_ssl_context_t* context = argument;
    if (!valid_context(context) || context->server_alpn == NULL ||
        SSL_select_next_proto((unsigned char**)out, out_len,
                              context->server_alpn, context->server_alpn_len,
                              in, in_len) != OPENSSL_NPN_NEGOTIATED) {
        return SSL_TLSEXT_ERR_NOACK;
    }
    return SSL_TLSEXT_ERR_OK;
}

static galay_status_t initialize_engine(galay_ssl_socket_t* socket)
{
    if (socket == NULL || !valid_context(socket->context)) {
        return GALAY_INVALID_ARGUMENT;
    }
    SSL* engine = SSL_new(socket->context->context);
    if (engine == NULL) {
        return GALAY_IO_ERROR;
    }
    BIO* read_bio = BIO_new(BIO_s_mem());
    BIO* write_bio = BIO_new(BIO_s_mem());
    if (read_bio == NULL || write_bio == NULL) {
        if (read_bio != NULL) (void)BIO_free(read_bio);
        if (write_bio != NULL) (void)BIO_free(write_bio);
        SSL_free(engine);
        return GALAY_OUT_OF_MEMORY;
    }
    if (BIO_set_mem_eof_return(read_bio, -1) <= 0 ||
        BIO_set_mem_eof_return(write_bio, -1) <= 0) {
        (void)BIO_free(read_bio);
        (void)BIO_free(write_bio);
        SSL_free(engine);
        return GALAY_IO_ERROR;
    }
    SSL_set_bio(engine, read_bio, write_bio);
    if (socket->context->method == GALAY_SSL_METHOD_TLS_SERVER) {
        SSL_set_accept_state(engine);
    } else {
        SSL_set_connect_state(engine);
    }
    socket->engine = engine;
    return GALAY_OK;
}

static C_IOResult send_all(galay_c_tcp_socket_t* transport,
                           const char* data,
                           size_t length,
                           int64_t timeout_ms)
{
    size_t sent = 0;
    while (sent < length) {
        C_IOResult result = galay_c_tcp_socket_send(
            transport, data + sent, length - sent, timeout_ms);
        if (result.code != C_IOResultOk) {
            result.bytes += sent;
            return result;
        }
        if (result.bytes == 0) {
            return make_io_result(C_IOResultEof, 0, sent);
        }
        sent += result.bytes;
    }
    return make_io_result(C_IOResultOk, 0, sent);
}

static C_IOResult flush_encrypted(galay_ssl_socket_t* socket, int64_t timeout_ms)
{
    unsigned char buffer[kCipherBufferSize];
    size_t total = 0;
    BIO* output = SSL_get_wbio(socket->engine);
    if (output == NULL) {
        return make_io_result(C_IOResultError, 0, 0);
    }
    while (BIO_ctrl_pending(output) > 0) {
        const int extracted = BIO_read(output, buffer, sizeof(buffer));
        if (extracted <= 0) {
            return make_io_result(C_IOResultError, 0, total);
        }
        C_IOResult sent = send_all(&socket->transport, (const char*)buffer,
                                   (size_t)extracted, timeout_ms);
        if (sent.code != C_IOResultOk) {
            return sent;
        }
        total += (size_t)extracted;
    }
    return make_io_result(C_IOResultOk, 0, total);
}

static C_IOResult receive_encrypted(galay_ssl_socket_t* socket, int64_t timeout_ms)
{
    unsigned char buffer[kCipherBufferSize];
    C_IOResult received = galay_c_tcp_socket_recv(
        &socket->transport, (char*)buffer, sizeof(buffer), timeout_ms);
    if (received.code != C_IOResultOk) {
        return received;
    }
    BIO* input = SSL_get_rbio(socket->engine);
    if (input == NULL || received.bytes > INT_MAX ||
        BIO_write(input, buffer, (int)received.bytes) != (int)received.bytes) {
        return make_io_result(C_IOResultError, 0, 0);
    }
    return received;
}

static C_IOResult ssl_error_result(SSL* engine, int operation_result)
{
    const int error = SSL_get_error(engine, operation_result);
    if (error == SSL_ERROR_ZERO_RETURN) {
        return make_io_result(C_IOResultEof, 0, 0);
    }
    if (error == SSL_ERROR_SYSCALL) {
        return make_io_result(C_IOResultError, errno, 0);
    }
    return make_io_result(C_IOResultError, 0, 0);
}

static C_IOResult drive_handshake(galay_ssl_socket_t* socket, int64_t timeout_ms)
{
    for (size_t step = 0; step < kMaxSslSteps; ++step) {
        ERR_clear_error();
        const int handshake = SSL_do_handshake(socket->engine);
        C_IOResult flushed = flush_encrypted(socket, timeout_ms);
        if (flushed.code != C_IOResultOk) {
            return flushed;
        }
        if (handshake == 1) {
            socket->handshake_complete = 1;
            return make_io_result(C_IOResultOk, 0, 0);
        }
        const int error = SSL_get_error(socket->engine, handshake);
        if (error == SSL_ERROR_WANT_READ) {
            C_IOResult received = receive_encrypted(socket, timeout_ms);
            if (received.code != C_IOResultOk) return received;
        } else if (error != SSL_ERROR_WANT_WRITE) {
            return ssl_error_result(socket->engine, handshake);
        }
    }
    return make_io_result(C_IOResultError, ELOOP, 0);
}

static galay_status_t copy_bytes(const unsigned char* value,
                                 size_t value_len,
                                 char* out,
                                 size_t out_len,
                                 size_t* written)
{
    if (written != NULL) {
        *written = value_len;
    }
    if (out == NULL || written == NULL || out_len < value_len ||
        (value == NULL && value_len != 0)) {
        return GALAY_INVALID_ARGUMENT;
    }
    if (value_len != 0) {
        memcpy(out, value, value_len);
    }
    return GALAY_OK;
}

const char* galay_ssl_get_error(galay_status_t status)
{
    return galay_status_string(status);
}

galay_status_t galay_ssl_context_create(galay_ssl_method_t method, galay_ssl_context_t** out)
{
    if (out == NULL || (method != GALAY_SSL_METHOD_TLS_CLIENT &&
                        method != GALAY_SSL_METHOD_TLS_SERVER)) {
        return GALAY_INVALID_ARGUMENT;
    }
    *out = NULL;
    galay_ssl_context_t* context = calloc(1, sizeof(*context));
    if (context == NULL) {
        return GALAY_OUT_OF_MEMORY;
    }
    const SSL_METHOD* ssl_method = method == GALAY_SSL_METHOD_TLS_SERVER
        ? TLS_server_method() : TLS_client_method();
    context->context = SSL_CTX_new(ssl_method);
    if (context->context == NULL) {
        free(context);
        return GALAY_IO_ERROR;
    }
    context->method = method;
    context->verify_mode = GALAY_SSL_VERIFY_NONE;
    *out = context;
    return GALAY_OK;
}

void galay_ssl_context_destroy(galay_ssl_context_t* context)
{
    if (context == NULL) return;
    SSL_CTX_free(context->context);
    free(context->server_alpn);
    free(context);
}

galay_status_t galay_ssl_context_load_certificate(galay_ssl_context_t* context, const char* path)
{
    if (!valid_context(context)) return GALAY_INVALID_ARGUMENT;
    const galay_status_t exists = file_exists(path);
    if (exists != GALAY_OK) return exists;
    return SSL_CTX_use_certificate_chain_file(context->context, path) == 1
        ? GALAY_OK : GALAY_IO_ERROR;
}

galay_status_t galay_ssl_context_load_private_key(galay_ssl_context_t* context, const char* path)
{
    if (!valid_context(context)) return GALAY_INVALID_ARGUMENT;
    const galay_status_t exists = file_exists(path);
    if (exists != GALAY_OK) return exists;
    return SSL_CTX_use_PrivateKey_file(context->context, path, SSL_FILETYPE_PEM) == 1
        ? GALAY_OK : GALAY_IO_ERROR;
}

galay_status_t galay_ssl_context_load_ca(galay_ssl_context_t* context, const char* path)
{
    if (!valid_context(context)) return GALAY_INVALID_ARGUMENT;
    const galay_status_t exists = file_exists(path);
    if (exists != GALAY_OK) return exists;
    return SSL_CTX_load_verify_locations(context->context, path, NULL) == 1
        ? GALAY_OK : GALAY_IO_ERROR;
}

galay_status_t galay_ssl_context_set_verify_mode(galay_ssl_context_t* context,
                                                 galay_ssl_verify_mode_t mode)
{
    if (!valid_context(context) ||
        (mode != GALAY_SSL_VERIFY_NONE && mode != GALAY_SSL_VERIFY_PEER)) {
        return GALAY_INVALID_ARGUMENT;
    }
    context->verify_mode = mode;
    SSL_CTX_set_verify(context->context,
                       mode == GALAY_SSL_VERIFY_PEER ? SSL_VERIFY_PEER : SSL_VERIFY_NONE,
                       NULL);
    return GALAY_OK;
}

galay_status_t galay_ssl_context_set_alpn_protocols(galay_ssl_context_t* context,
                                                    const char* const* protocols,
                                                    size_t count)
{
    if (!valid_context(context)) return GALAY_INVALID_ARGUMENT;
    unsigned char* wire = NULL;
    unsigned int wire_len = 0;
    const galay_status_t encoded = make_alpn_wire(protocols, count, &wire, &wire_len);
    if (encoded != GALAY_OK) return encoded;
    const int configured = SSL_CTX_set_alpn_protos(context->context, wire, wire_len);
    free(wire);
    return configured == 0 ? GALAY_OK : GALAY_IO_ERROR;
}

galay_status_t galay_ssl_context_set_alpn_select_protocols(galay_ssl_context_t* context,
                                                           const char* const* protocols,
                                                           size_t count)
{
    if (!valid_context(context)) return GALAY_INVALID_ARGUMENT;
    unsigned char* wire = NULL;
    unsigned int wire_len = 0;
    const galay_status_t encoded = make_alpn_wire(protocols, count, &wire, &wire_len);
    if (encoded != GALAY_OK) return encoded;
    free(context->server_alpn);
    context->server_alpn = wire;
    context->server_alpn_len = wire_len;
    SSL_CTX_set_alpn_select_cb(context->context, select_alpn, context);
    return GALAY_OK;
}

galay_status_t galay_ssl_context_set_session_cache_mode(galay_ssl_context_t* context,
                                                        galay_ssl_session_cache_mode_t mode)
{
    long value = 0;
    if (!valid_context(context) || !session_cache_mode(mode, &value)) {
        return GALAY_INVALID_ARGUMENT;
    }
    (void)SSL_CTX_set_session_cache_mode(context->context, value);
    return GALAY_OK;
}

galay_status_t galay_ssl_context_set_session_timeout(galay_ssl_context_t* context,
                                                     long timeout_seconds)
{
    if (!valid_context(context) || timeout_seconds < 0) return GALAY_INVALID_ARGUMENT;
    (void)SSL_CTX_set_timeout(context->context, timeout_seconds);
    return GALAY_OK;
}

galay_status_t galay_ssl_context_disable_session_cache(galay_ssl_context_t* context)
{
    return galay_ssl_context_set_session_cache_mode(context, GALAY_SSL_SESSION_CACHE_OFF);
}

galay_status_t galay_ssl_context_disable_session_tickets(galay_ssl_context_t* context)
{
    if (!valid_context(context)) return GALAY_INVALID_ARGUMENT;
    return (SSL_CTX_set_options(context->context, SSL_OP_NO_TICKET) & SSL_OP_NO_TICKET) != 0
        ? GALAY_OK : GALAY_IO_ERROR;
}

galay_status_t galay_ssl_socket_create(galay_ssl_context_t* context, C_IPType type,
                                       galay_ssl_socket_t** out)
{
    if (!valid_context(context) || !valid_ip_type(type) || out == NULL) {
        return GALAY_INVALID_ARGUMENT;
    }
    *out = NULL;
    galay_ssl_socket_t* socket = calloc(1, sizeof(*socket));
    if (socket == NULL) return GALAY_OUT_OF_MEMORY;
    socket->transport.fd = -1;
    socket->context = context;
    C_IOResult created = galay_c_tcp_socket_create(&socket->transport, type);
    if (created.code != C_IOResultOk) {
        free(socket);
        return GALAY_IO_ERROR;
    }
    const galay_status_t initialized = initialize_engine(socket);
    if (initialized != GALAY_OK) {
        const C_IOResult closed = galay_c_tcp_socket_close(&socket->transport);
        free(socket);
        return closed.code == C_IOResultOk ? initialized : GALAY_IO_ERROR;
    }
    *out = socket;
    return GALAY_OK;
}

void galay_ssl_socket_destroy(galay_ssl_socket_t* socket)
{
    if (socket == NULL) return;
    SSL_free(socket->engine);
    if (socket->transport.fd >= 0) {
        const C_IOResult closed = galay_c_tcp_socket_close(&socket->transport);
        (void)closed; /* void destroy cannot propagate an OS close failure. */
    }
    free(socket);
}

galay_status_t galay_ssl_socket_bind(galay_ssl_socket_t* socket, const C_Host* host)
{
    if (!valid_socket(socket) || host == NULL) return GALAY_INVALID_ARGUMENT;
    const C_IOResult bound = galay_c_tcp_socket_bind(&socket->transport, host);
    return bound.code == C_IOResultOk ? GALAY_OK :
        (bound.code == C_IOResultInvalid ? GALAY_INVALID_ARGUMENT : GALAY_IO_ERROR);
}

galay_status_t galay_ssl_socket_listen(galay_ssl_socket_t* socket, int backlog)
{
    if (!valid_socket(socket)) return GALAY_INVALID_ARGUMENT;
    const C_IOResult listened = galay_c_tcp_socket_listen(&socket->transport, backlog);
    return listened.code == C_IOResultOk ? GALAY_OK :
        (listened.code == C_IOResultInvalid ? GALAY_INVALID_ARGUMENT : GALAY_IO_ERROR);
}

galay_status_t galay_ssl_socket_local_endpoint(const galay_ssl_socket_t* socket, C_Host* out)
{
    if (!valid_socket(socket) || out == NULL) return GALAY_INVALID_ARGUMENT;
    const C_IOResult endpoint = galay_c_tcp_socket_local_endpoint(&socket->transport, out);
    return endpoint.code == C_IOResultOk ? GALAY_OK :
        (endpoint.code == C_IOResultInvalid ? GALAY_INVALID_ARGUMENT : GALAY_IO_ERROR);
}

galay_status_t galay_ssl_socket_set_hostname(galay_ssl_socket_t* socket, const char* hostname)
{
    if (!valid_socket(socket) || hostname == NULL || hostname[0] == '\0' ||
        socket->handshake_complete) {
        return GALAY_INVALID_ARGUMENT;
    }
    return SSL_set_tlsext_host_name(socket->engine, hostname) == 1
        ? GALAY_OK : GALAY_IO_ERROR;
}

C_IOResult galay_ssl_socket_accept(galay_ssl_socket_t* listener, galay_ssl_socket_t** out,
                                   C_Host* out_peer, int64_t timeout_ms)
{
    if (!valid_socket(listener) || out == NULL || *out != NULL) {
        return make_io_result(C_IOResultInvalid, 0, 0);
    }
    galay_c_tcp_socket_t accepted_transport;
    memset(&accepted_transport, 0, sizeof(accepted_transport));
    accepted_transport.fd = -1;
    C_IOResult accepted = galay_c_tcp_socket_accept(
        &listener->transport, &accepted_transport, out_peer, timeout_ms);
    if (accepted.code != C_IOResultOk) return accepted;
    galay_ssl_socket_t* socket = calloc(1, sizeof(*socket));
    if (socket == NULL) {
        const C_IOResult closed = galay_c_tcp_socket_close(&accepted_transport);
        return closed.code == C_IOResultOk
            ? make_io_result(C_IOResultError, ENOMEM, 0) : closed;
    }
    socket->context = listener->context;
    socket->transport = accepted_transport;
    const galay_status_t initialized = initialize_engine(socket);
    if (initialized != GALAY_OK) {
        const C_IOResult closed = galay_c_tcp_socket_close(&socket->transport);
        free(socket);
        return closed.code == C_IOResultOk
            ? make_io_result(C_IOResultError, 0, 0) : closed;
    }
    *out = socket;
    accepted.ptr = socket;
    return accepted;
}

C_IOResult galay_ssl_socket_connect(galay_ssl_socket_t* socket, const C_Host* host,
                                    int64_t timeout_ms)
{
    if (!valid_socket(socket) || host == NULL) {
        return make_io_result(C_IOResultInvalid, 0, 0);
    }
    return galay_c_tcp_socket_connect(&socket->transport, host, timeout_ms);
}

C_IOResult galay_ssl_socket_handshake(galay_ssl_socket_t* socket, int64_t timeout_ms)
{
    if (!valid_socket(socket) || timeout_ms < -1) {
        return make_io_result(C_IOResultInvalid, 0, 0);
    }
    return socket->handshake_complete
        ? make_io_result(C_IOResultOk, 0, 0)
        : drive_handshake(socket, timeout_ms);
}

C_IOResult galay_ssl_socket_recv(galay_ssl_socket_t* socket, char* buffer, size_t length,
                                 int64_t timeout_ms)
{
    if (!valid_socket(socket) || !socket->handshake_complete || buffer == NULL ||
        length == 0 || timeout_ms < -1) {
        return make_io_result(C_IOResultInvalid, 0, 0);
    }
    for (size_t step = 0; step < kMaxSslSteps; ++step) {
        size_t bytes = 0;
        ERR_clear_error();
        const int read_ok = SSL_read_ex(socket->engine, buffer, length, &bytes);
        if (read_ok == 1 && bytes != 0) {
            return make_io_result(C_IOResultOk, 0, bytes);
        }
        const int error = SSL_get_error(socket->engine, read_ok);
        if (error == SSL_ERROR_WANT_WRITE) {
            C_IOResult flushed = flush_encrypted(socket, timeout_ms);
            if (flushed.code != C_IOResultOk) return flushed;
        } else if (error == SSL_ERROR_WANT_READ) {
            C_IOResult flushed = flush_encrypted(socket, timeout_ms);
            if (flushed.code != C_IOResultOk) return flushed;
            C_IOResult received = receive_encrypted(socket, timeout_ms);
            if (received.code != C_IOResultOk) return received;
        } else {
            return ssl_error_result(socket->engine, read_ok);
        }
    }
    return make_io_result(C_IOResultError, ELOOP, 0);
}

C_IOResult galay_ssl_socket_send(galay_ssl_socket_t* socket, const char* buffer, size_t length,
                                 int64_t timeout_ms)
{
    if (!valid_socket(socket) || !socket->handshake_complete || buffer == NULL ||
        length == 0 || timeout_ms < -1) {
        return make_io_result(C_IOResultInvalid, 0, 0);
    }
    size_t total = 0;
    for (size_t step = 0; step < kMaxSslSteps && total < length; ++step) {
        size_t bytes = 0;
        ERR_clear_error();
        const int write_ok = SSL_write_ex(socket->engine, buffer + total, length - total, &bytes);
        if (write_ok == 1 && bytes != 0) {
            total += bytes;
            C_IOResult flushed = flush_encrypted(socket, timeout_ms);
            if (flushed.code != C_IOResultOk) {
                flushed.bytes = total;
                return flushed;
            }
            continue;
        }
        const int error = SSL_get_error(socket->engine, write_ok);
        if (error == SSL_ERROR_WANT_WRITE) {
            C_IOResult flushed = flush_encrypted(socket, timeout_ms);
            if (flushed.code != C_IOResultOk) return flushed;
        } else if (error == SSL_ERROR_WANT_READ) {
            C_IOResult received = receive_encrypted(socket, timeout_ms);
            if (received.code != C_IOResultOk) return received;
        } else {
            C_IOResult result = ssl_error_result(socket->engine, write_ok);
            result.bytes = total;
            return result;
        }
    }
    return total == length ? make_io_result(C_IOResultOk, 0, total)
                           : make_io_result(C_IOResultError, ELOOP, total);
}

C_IOResult galay_ssl_socket_shutdown(galay_ssl_socket_t* socket, int64_t timeout_ms)
{
    if (!valid_socket(socket) || timeout_ms < -1) {
        return make_io_result(C_IOResultInvalid, 0, 0);
    }
    for (size_t step = 0; step < kMaxSslSteps; ++step) {
        ERR_clear_error();
        const int shutdown_result = SSL_shutdown(socket->engine);
        C_IOResult flushed = flush_encrypted(socket, timeout_ms);
        if (flushed.code != C_IOResultOk) return flushed;
        if (shutdown_result == 1) return make_io_result(C_IOResultOk, 0, 0);
        const int error = SSL_get_error(socket->engine, shutdown_result);
        if (shutdown_result == 0 || error == SSL_ERROR_WANT_READ) {
            C_IOResult received = receive_encrypted(socket, timeout_ms);
            if (received.code == C_IOResultEof) return make_io_result(C_IOResultOk, 0, 0);
            if (received.code != C_IOResultOk) return received;
        } else if (error != SSL_ERROR_WANT_WRITE) {
            return ssl_error_result(socket->engine, shutdown_result);
        }
    }
    return make_io_result(C_IOResultError, ELOOP, 0);
}

C_IOResult galay_ssl_socket_close(galay_ssl_socket_t* socket, int64_t timeout_ms)
{
    if (socket == NULL || timeout_ms < -1 || socket->transport.fd < 0) {
        return make_io_result(C_IOResultInvalid, 0, 0);
    }
    return galay_c_tcp_socket_close(&socket->transport);
}

galay_status_t galay_ssl_socket_get_protocol(const galay_ssl_socket_t* socket, char* out,
                                             size_t out_len, size_t* written)
{
    if (!valid_socket(socket)) return GALAY_INVALID_ARGUMENT;
    const char* value = SSL_get_version(socket->engine);
    return copy_bytes((const unsigned char*)value, value == NULL ? 0 : strlen(value),
                      out, out_len, written);
}

galay_status_t galay_ssl_socket_get_cipher(const galay_ssl_socket_t* socket, char* out,
                                           size_t out_len, size_t* written)
{
    if (!valid_socket(socket)) return GALAY_INVALID_ARGUMENT;
    const char* value = SSL_get_cipher_name(socket->engine);
    return copy_bytes((const unsigned char*)value, value == NULL ? 0 : strlen(value),
                      out, out_len, written);
}

galay_status_t galay_ssl_socket_get_negotiated_alpn(const galay_ssl_socket_t* socket, char* out,
                                                    size_t out_len, size_t* written)
{
    if (!valid_socket(socket)) return GALAY_INVALID_ARGUMENT;
    const unsigned char* value = NULL;
    unsigned int value_len = 0;
    SSL_get0_alpn_selected(socket->engine, &value, &value_len);
    return copy_bytes(value, value_len, out, out_len, written);
}
