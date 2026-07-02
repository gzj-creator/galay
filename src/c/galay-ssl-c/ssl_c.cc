#include <galay/c/galay-ssl-c/ssl_c.h>

#include <galay/cpp/galay-ssl/ssl/ssl_context.h>
#include <galay/cpp/galay-ssl/ssl/ssl_engine.h>

#include <openssl/ssl.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <new>
#include <string>
#include <vector>

struct galay_ssl_context_t {
    galay_ssl_method_t method = GALAY_SSL_METHOD_TLS_CLIENT;
    galay_ssl_verify_mode_t verify_mode = GALAY_SSL_VERIFY_NONE;
    galay::ssl::SslContext* context = nullptr;
};

struct galay_ssl_socket_t {
    galay_ssl_context_t* context = nullptr;
    galay_kernel_tcp_socket_t transport{nullptr};
    galay::ssl::SslEngine* engine = nullptr;
    bool handshake_complete = false;
};

namespace
{

constexpr size_t kCipherBufferSize = 16 * 1024;

C_IOResult make_io_result(C_IOResultCode code, int sys_errno = 0, size_t bytes = 0)
{
    return C_IOResult{code, sys_errno, bytes, 0, nullptr};
}

bool is_valid_ip_type(C_IPType type)
{
    return type == C_IPTypeIPV4 || type == C_IPTypeIPV6;
}

bool is_server_context(const galay_ssl_context_t* context)
{
    return context != nullptr && context->method == GALAY_SSL_METHOD_TLS_SERVER;
}

galay::ssl::SslMethod to_cpp_method(galay_ssl_method_t method)
{
    return method == GALAY_SSL_METHOD_TLS_SERVER
        ? galay::ssl::SslMethod::TLS_Server
        : galay::ssl::SslMethod::TLS_Client;
}

galay::ssl::SslVerifyMode to_cpp_verify_mode(galay_ssl_verify_mode_t mode)
{
    return mode == GALAY_SSL_VERIFY_PEER
        ? galay::ssl::SslVerifyMode::Peer
        : galay::ssl::SslVerifyMode::None;
}

bool to_cpp_session_cache_mode(galay_ssl_session_cache_mode_t mode, long* out)
{
    if (out == nullptr) {
        return false;
    }
    switch (mode) {
    case GALAY_SSL_SESSION_CACHE_OFF:
        *out = SSL_SESS_CACHE_OFF;
        return true;
    case GALAY_SSL_SESSION_CACHE_CLIENT:
        *out = SSL_SESS_CACHE_CLIENT;
        return true;
    case GALAY_SSL_SESSION_CACHE_SERVER:
        *out = SSL_SESS_CACHE_SERVER;
        return true;
    case GALAY_SSL_SESSION_CACHE_BOTH:
        *out = SSL_SESS_CACHE_BOTH;
        return true;
    }
    return false;
}

bool valid_protocol_list(const char* const* protocols, size_t count)
{
    if (protocols == nullptr || count == 0) {
        return false;
    }
    for (size_t i = 0; i < count; ++i) {
        if (protocols[i] == nullptr) {
            return false;
        }
        const size_t length = std::strlen(protocols[i]);
        if (length == 0 || length > 255) {
            return false;
        }
    }
    return true;
}

std::vector<std::string> make_protocol_list(const char* const* protocols, size_t count)
{
    std::vector<std::string> result;
    result.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        result.emplace_back(protocols[i]);
    }
    return result;
}

galay_status_t file_exists(const char* path)
{
    if (path == nullptr || path[0] == '\0') {
        return GALAY_INVALID_ARGUMENT;
    }
    FILE* file = std::fopen(path, "rb");
    if (file == nullptr) {
        return GALAY_NOT_FOUND;
    }
    const int close_result = std::fclose(file);
    return close_result == 0 ? GALAY_OK : GALAY_IO_ERROR;
}

galay_status_t map_ssl_expected(bool ok)
{
    return ok ? GALAY_OK : GALAY_IO_ERROR;
}

C_IOResult map_ssl_io_error(galay::ssl::SslIOResult result)
{
    switch (result) {
    case galay::ssl::SslIOResult::ZeroReturn:
        return make_io_result(C_IOResultEof);
    case galay::ssl::SslIOResult::WantRead:
    case galay::ssl::SslIOResult::WantWrite:
    case galay::ssl::SslIOResult::Error:
    case galay::ssl::SslIOResult::Syscall:
        return make_io_result(C_IOResultError);
    case galay::ssl::SslIOResult::Success:
        return make_io_result(C_IOResultOk);
    }
    return make_io_result(C_IOResultError);
}

bool valid_socket(const galay_ssl_socket_t* socket)
{
    return socket != nullptr && socket->context != nullptr &&
        socket->context->context != nullptr && socket->transport.socket != nullptr;
}

bool valid_engine_socket(const galay_ssl_socket_t* socket)
{
    return valid_socket(socket) && socket->engine != nullptr && socket->engine->isValid();
}

galay_status_t init_engine(galay_ssl_socket_t* socket)
{
    if (!valid_socket(socket)) {
        return GALAY_INVALID_ARGUMENT;
    }
    if (socket->engine != nullptr) {
        return GALAY_OK;
    }

    auto* engine = new (std::nothrow) galay::ssl::SslEngine(socket->context->context);
    if (engine == nullptr) {
        return GALAY_OUT_OF_MEMORY;
    }
    if (!engine->isValid()) {
        delete engine;
        return GALAY_IO_ERROR;
    }
    auto bio = engine->initMemoryBIO();
    if (!bio) {
        delete engine;
        return GALAY_IO_ERROR;
    }
    if (is_server_context(socket->context)) {
        engine->setAcceptState();
    } else {
        engine->setConnectState();
    }
    socket->engine = engine;
    return GALAY_OK;
}

C_IOResult send_all_plain_tcp(galay_kernel_tcp_socket_t* transport,
                              const char* data,
                              size_t length,
                              int64_t timeout_ms)
{
    size_t sent = 0;
    while (sent < length) {
        C_IOResult result =
            galay_kernel_tcp_socket_send(transport, data + sent, length - sent, timeout_ms);
        if (result.code != C_IOResultOk) {
            return result;
        }
        if (result.bytes == 0) {
            return make_io_result(C_IOResultError);
        }
        sent += result.bytes;
    }
    return make_io_result(C_IOResultOk, 0, sent);
}

C_IOResult flush_encrypted(galay_ssl_socket_t* socket, int64_t timeout_ms)
{
    std::array<char, kCipherBufferSize> buffer{};
    size_t total = 0;
    while (socket->engine->pendingEncryptedOutput() > 0) {
        const size_t pending = socket->engine->pendingEncryptedOutput();
        const size_t chunk = std::min(pending, buffer.size());
        const int extracted = socket->engine->extractEncryptedOutput(buffer.data(), chunk);
        if (extracted <= 0) {
            return make_io_result(C_IOResultError);
        }
        C_IOResult sent = send_all_plain_tcp(
            &socket->transport, buffer.data(), static_cast<size_t>(extracted), timeout_ms);
        if (sent.code != C_IOResultOk) {
            return sent;
        }
        total += static_cast<size_t>(extracted);
    }
    return make_io_result(C_IOResultOk, 0, total);
}

C_IOResult recv_encrypted(galay_ssl_socket_t* socket, int64_t timeout_ms)
{
    std::array<char, kCipherBufferSize> buffer{};
    C_IOResult received =
        galay_kernel_tcp_socket_recv(&socket->transport, buffer.data(), buffer.size(), timeout_ms);
    if (received.code != C_IOResultOk) {
        return received;
    }
    if (received.bytes == 0) {
        return make_io_result(C_IOResultEof);
    }
    const int fed = socket->engine->feedEncryptedInput(buffer.data(), received.bytes);
    if (fed <= 0 || static_cast<size_t>(fed) != received.bytes) {
        return make_io_result(C_IOResultError);
    }
    return received;
}

C_IOResult drive_handshake(galay_ssl_socket_t* socket, int64_t timeout_ms)
{
    for (size_t i = 0; i < 4096; ++i) {
        const galay::ssl::SslIOResult step = socket->engine->doHandshake();
        C_IOResult flushed = flush_encrypted(socket, timeout_ms);
        if (flushed.code != C_IOResultOk) {
            return flushed;
        }
        if (step == galay::ssl::SslIOResult::Success) {
            socket->handshake_complete = true;
            return make_io_result(C_IOResultOk);
        }
        if (step == galay::ssl::SslIOResult::WantRead) {
            C_IOResult received = recv_encrypted(socket, timeout_ms);
            if (received.code != C_IOResultOk) {
                return received;
            }
            continue;
        }
        if (step == galay::ssl::SslIOResult::WantWrite) {
            continue;
        }
        return map_ssl_io_error(step);
    }
    return make_io_result(C_IOResultError);
}

C_IOResult copy_string_result(const std::string& value, char* out, size_t out_len, size_t* written)
{
    if (written != nullptr) {
        *written = value.size();
    }
    if (out == nullptr || written == nullptr) {
        return make_io_result(C_IOResultInvalid);
    }
    if (out_len < value.size()) {
        return make_io_result(C_IOResultInvalid);
    }
    if (!value.empty()) {
        std::memcpy(out, value.data(), value.size());
    }
    return make_io_result(C_IOResultOk, 0, value.size());
}

galay_status_t copy_string_status(const std::string& value, char* out, size_t out_len, size_t* written)
{
    const C_IOResult result = copy_string_result(value, out, out_len, written);
    return result.code == C_IOResultOk ? GALAY_OK : GALAY_INVALID_ARGUMENT;
}

} // namespace

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
    *out = nullptr;

    auto* context = new (std::nothrow) galay_ssl_context_t();
    if (context == nullptr) {
        return GALAY_OUT_OF_MEMORY;
    }
    context->context = new (std::nothrow) galay::ssl::SslContext(to_cpp_method(method));
    if (context->context == nullptr) {
        delete context;
        return GALAY_OUT_OF_MEMORY;
    }
    if (!context->context->isValid()) {
        delete context->context;
        delete context;
        return GALAY_IO_ERROR;
    }
    context->method = method;
    *out = context;
    return GALAY_OK;
}

void galay_ssl_context_destroy(galay_ssl_context_t* context)
{
    if (context == nullptr) {
        return;
    }
    delete context->context;
    context->context = nullptr;
    delete context;
}

galay_status_t galay_ssl_context_load_certificate(galay_ssl_context_t* context, const char* path)
{
    if (context == nullptr || context->context == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    const galay_status_t exists = file_exists(path);
    if (exists != GALAY_OK) {
        return exists;
    }
    auto loaded = context->context->loadCertificate(path);
    return map_ssl_expected(loaded.has_value());
}

galay_status_t galay_ssl_context_load_private_key(galay_ssl_context_t* context, const char* path)
{
    if (context == nullptr || context->context == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    const galay_status_t exists = file_exists(path);
    if (exists != GALAY_OK) {
        return exists;
    }
    auto loaded = context->context->loadPrivateKey(path);
    return map_ssl_expected(loaded.has_value());
}

galay_status_t galay_ssl_context_load_ca(galay_ssl_context_t* context, const char* path)
{
    if (context == nullptr || context->context == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    const galay_status_t exists = file_exists(path);
    if (exists != GALAY_OK) {
        return exists;
    }
    auto loaded = context->context->loadCACertificate(path);
    return map_ssl_expected(loaded.has_value());
}

galay_status_t galay_ssl_context_set_verify_mode(galay_ssl_context_t* context,
                                                 galay_ssl_verify_mode_t mode)
{
    if (context == nullptr || context->context == nullptr ||
        (mode != GALAY_SSL_VERIFY_NONE && mode != GALAY_SSL_VERIFY_PEER)) {
        return GALAY_INVALID_ARGUMENT;
    }
    context->verify_mode = mode;
    context->context->setVerifyMode(to_cpp_verify_mode(mode));
    return GALAY_OK;
}

galay_status_t galay_ssl_context_set_alpn_protocols(galay_ssl_context_t* context,
                                                    const char* const* protocols,
                                                    size_t count)
{
    if (context == nullptr || context->context == nullptr ||
        !valid_protocol_list(protocols, count)) {
        return GALAY_INVALID_ARGUMENT;
    }
    auto configured = context->context->setALPNProtocols(make_protocol_list(protocols, count));
    return map_ssl_expected(configured.has_value());
}

galay_status_t galay_ssl_context_set_alpn_select_protocols(galay_ssl_context_t* context,
                                                           const char* const* protocols,
                                                           size_t count)
{
    if (context == nullptr || context->context == nullptr ||
        !valid_protocol_list(protocols, count)) {
        return GALAY_INVALID_ARGUMENT;
    }
    auto configured = context->context->setALPNSelectProtocols(make_protocol_list(protocols, count));
    return map_ssl_expected(configured.has_value());
}

galay_status_t galay_ssl_context_set_session_cache_mode(galay_ssl_context_t* context,
                                                        galay_ssl_session_cache_mode_t mode)
{
    long cpp_mode = 0;
    if (context == nullptr || context->context == nullptr ||
        !to_cpp_session_cache_mode(mode, &cpp_mode)) {
        return GALAY_INVALID_ARGUMENT;
    }
    context->context->setSessionCacheMode(cpp_mode);
    return GALAY_OK;
}

galay_status_t galay_ssl_context_set_session_timeout(galay_ssl_context_t* context,
                                                     long timeout_seconds)
{
    if (context == nullptr || context->context == nullptr || timeout_seconds < 0) {
        return GALAY_INVALID_ARGUMENT;
    }
    context->context->setSessionTimeout(timeout_seconds);
    return GALAY_OK;
}

galay_status_t galay_ssl_context_disable_session_cache(galay_ssl_context_t* context)
{
    if (context == nullptr || context->context == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    context->context->disableSessionCache();
    return GALAY_OK;
}

galay_status_t galay_ssl_context_disable_session_tickets(galay_ssl_context_t* context)
{
    if (context == nullptr || context->context == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    context->context->disableSessionTickets();
    return GALAY_OK;
}

galay_status_t galay_ssl_socket_create(galay_ssl_context_t* context, C_IPType type,
                                       galay_ssl_socket_t** out)
{
    if (context == nullptr || context->context == nullptr || out == nullptr || !is_valid_ip_type(type)) {
        return GALAY_INVALID_ARGUMENT;
    }
    *out = nullptr;

    auto* socket = new (std::nothrow) galay_ssl_socket_t();
    if (socket == nullptr) {
        return GALAY_OUT_OF_MEMORY;
    }
    socket->context = context;
    C_TcpSocketResultCode created = galay_kernel_tcp_socket_create(&socket->transport, type);
    if (created != C_TcpSocketSuccess) {
        delete socket;
        return created == C_TcpSocketParameterInvalid ? GALAY_INVALID_ARGUMENT : GALAY_IO_ERROR;
    }
    const galay_status_t engine_status = init_engine(socket);
    if (engine_status != GALAY_OK) {
        C_TcpSocketResultCode destroyed = galay_kernel_tcp_socket_destroy(&socket->transport);
        if (destroyed != C_TcpSocketSuccess) {
            delete socket;
            return GALAY_IO_ERROR;
        }
        delete socket;
        return engine_status;
    }
    *out = socket;
    return GALAY_OK;
}

void galay_ssl_socket_destroy(galay_ssl_socket_t* socket)
{
    if (socket == nullptr) {
        return;
    }
    delete socket->engine;
    socket->engine = nullptr;
    if (socket->transport.socket != nullptr) {
        const C_TcpSocketResultCode destroyed = galay_kernel_tcp_socket_destroy(&socket->transport);
        if (destroyed != C_TcpSocketSuccess) {
            socket->transport.socket = nullptr;
        }
    }
    delete socket;
}

galay_status_t galay_ssl_socket_bind(galay_ssl_socket_t* socket, const C_Host* host)
{
    if (!valid_socket(socket) || host == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    const C_TcpSocketResultCode bound = galay_kernel_tcp_socket_bind(&socket->transport, host);
    return bound == C_TcpSocketSuccess ? GALAY_OK :
        (bound == C_TcpSocketParameterInvalid ? GALAY_INVALID_ARGUMENT : GALAY_IO_ERROR);
}

galay_status_t galay_ssl_socket_listen(galay_ssl_socket_t* socket, int backlog)
{
    if (!valid_socket(socket)) {
        return GALAY_INVALID_ARGUMENT;
    }
    const C_TcpSocketResultCode listened = galay_kernel_tcp_socket_listen(&socket->transport, backlog);
    return listened == C_TcpSocketSuccess ? GALAY_OK :
        (listened == C_TcpSocketParameterInvalid ? GALAY_INVALID_ARGUMENT : GALAY_IO_ERROR);
}

galay_status_t galay_ssl_socket_local_endpoint(const galay_ssl_socket_t* socket, C_Host* out)
{
    if (!valid_socket(socket) || out == nullptr) {
        return GALAY_INVALID_ARGUMENT;
    }
    const C_TcpSocketResultCode endpoint =
        galay_kernel_tcp_socket_local_endpoint(&socket->transport, out);
    return endpoint == C_TcpSocketSuccess ? GALAY_OK :
        (endpoint == C_TcpSocketParameterInvalid ? GALAY_INVALID_ARGUMENT : GALAY_IO_ERROR);
}

galay_status_t galay_ssl_socket_set_hostname(galay_ssl_socket_t* socket, const char* hostname)
{
    if (!valid_engine_socket(socket) || hostname == nullptr || hostname[0] == '\0') {
        return GALAY_INVALID_ARGUMENT;
    }
    auto result = socket->engine->setHostname(hostname);
    return map_ssl_expected(result.has_value());
}

C_IOResult galay_ssl_socket_accept(galay_ssl_socket_t* listener, galay_ssl_socket_t** out,
                                   C_Host* out_peer, int64_t timeout_ms)
{
    if (!valid_socket(listener) || out == nullptr || *out != nullptr) {
        return make_io_result(C_IOResultInvalid);
    }
    galay_kernel_tcp_socket_t accepted_transport{nullptr};
    C_IOResult accepted =
        galay_kernel_tcp_socket_accept(&listener->transport, &accepted_transport, out_peer, timeout_ms);
    if (accepted.code != C_IOResultOk) {
        return accepted;
    }

    auto* socket = new (std::nothrow) galay_ssl_socket_t();
    if (socket == nullptr) {
        C_TcpSocketResultCode destroyed = galay_kernel_tcp_socket_destroy(&accepted_transport);
        if (destroyed != C_TcpSocketSuccess) {
            accepted.code = C_IOResultError;
            return accepted;
        }
        return make_io_result(C_IOResultError);
    }
    socket->context = listener->context;
    socket->transport = accepted_transport;
    const galay_status_t engine_status = init_engine(socket);
    if (engine_status != GALAY_OK) {
        C_TcpSocketResultCode destroyed = galay_kernel_tcp_socket_destroy(&socket->transport);
        if (destroyed != C_TcpSocketSuccess) {
            delete socket;
            return make_io_result(C_IOResultError);
        }
        delete socket;
        return make_io_result(C_IOResultError);
    }
    *out = socket;
    accepted.ptr = socket;
    return accepted;
}

C_IOResult galay_ssl_socket_connect(galay_ssl_socket_t* socket, const C_Host* host,
                                    int64_t timeout_ms)
{
    if (!valid_engine_socket(socket) || host == nullptr) {
        return make_io_result(C_IOResultInvalid);
    }
    return galay_kernel_tcp_socket_connect(&socket->transport, host, timeout_ms);
}

C_IOResult galay_ssl_socket_handshake(galay_ssl_socket_t* socket, int64_t timeout_ms)
{
    if (!valid_engine_socket(socket)) {
        return make_io_result(C_IOResultInvalid);
    }
    if (socket->handshake_complete) {
        return make_io_result(C_IOResultOk);
    }
    return drive_handshake(socket, timeout_ms);
}

C_IOResult galay_ssl_socket_recv(galay_ssl_socket_t* socket, char* buffer, size_t length,
                                 int64_t timeout_ms)
{
    if (!valid_engine_socket(socket) || buffer == nullptr || length == 0 || !socket->handshake_complete) {
        return make_io_result(C_IOResultInvalid);
    }

    for (size_t i = 0; i < 4096; ++i) {
        size_t bytes_read = 0;
        const galay::ssl::SslIOResult step = socket->engine->read(buffer, length, bytes_read);
        if (step == galay::ssl::SslIOResult::Success && bytes_read > 0) {
            return make_io_result(C_IOResultOk, 0, bytes_read);
        }
        if (step == galay::ssl::SslIOResult::ZeroReturn) {
            return make_io_result(C_IOResultEof);
        }
        if (step == galay::ssl::SslIOResult::WantWrite) {
            C_IOResult flushed = flush_encrypted(socket, timeout_ms);
            if (flushed.code != C_IOResultOk) {
                return flushed;
            }
            continue;
        }
        if (step == galay::ssl::SslIOResult::WantRead) {
            C_IOResult flushed = flush_encrypted(socket, timeout_ms);
            if (flushed.code != C_IOResultOk) {
                return flushed;
            }
            C_IOResult received = recv_encrypted(socket, timeout_ms);
            if (received.code != C_IOResultOk) {
                return received;
            }
            continue;
        }
        return map_ssl_io_error(step);
    }
    return make_io_result(C_IOResultError);
}

C_IOResult galay_ssl_socket_send(galay_ssl_socket_t* socket, const char* buffer, size_t length,
                                 int64_t timeout_ms)
{
    if (!valid_engine_socket(socket) || buffer == nullptr || length == 0 || !socket->handshake_complete) {
        return make_io_result(C_IOResultInvalid);
    }

    size_t written_total = 0;
    for (size_t i = 0; i < 4096 && written_total < length; ++i) {
        size_t bytes_written = 0;
        const galay::ssl::SslIOResult step =
            socket->engine->write(buffer + written_total, length - written_total, bytes_written);
        if (step == galay::ssl::SslIOResult::Success && bytes_written > 0) {
            written_total += bytes_written;
            C_IOResult flushed = flush_encrypted(socket, timeout_ms);
            if (flushed.code != C_IOResultOk) {
                return flushed;
            }
            continue;
        }
        if (step == galay::ssl::SslIOResult::WantWrite) {
            C_IOResult flushed = flush_encrypted(socket, timeout_ms);
            if (flushed.code != C_IOResultOk) {
                return flushed;
            }
            continue;
        }
        if (step == galay::ssl::SslIOResult::WantRead) {
            C_IOResult received = recv_encrypted(socket, timeout_ms);
            if (received.code != C_IOResultOk) {
                return received;
            }
            continue;
        }
        return map_ssl_io_error(step);
    }
    return written_total == length
        ? make_io_result(C_IOResultOk, 0, written_total)
        : make_io_result(C_IOResultError, 0, written_total);
}

C_IOResult galay_ssl_socket_shutdown(galay_ssl_socket_t* socket, int64_t timeout_ms)
{
    if (!valid_engine_socket(socket)) {
        return make_io_result(C_IOResultInvalid);
    }

    for (size_t i = 0; i < 4096; ++i) {
        const galay::ssl::SslIOResult step = socket->engine->shutdown();
        C_IOResult flushed = flush_encrypted(socket, timeout_ms);
        if (flushed.code != C_IOResultOk) {
            return flushed;
        }
        if (step == galay::ssl::SslIOResult::Success || step == galay::ssl::SslIOResult::ZeroReturn) {
            return make_io_result(C_IOResultOk);
        }
        if (step == galay::ssl::SslIOResult::WantRead) {
            C_IOResult received = recv_encrypted(socket, timeout_ms);
            if (received.code == C_IOResultEof) {
                return make_io_result(C_IOResultOk);
            }
            if (received.code != C_IOResultOk) {
                return received;
            }
            continue;
        }
        if (step == galay::ssl::SslIOResult::WantWrite) {
            continue;
        }
        return map_ssl_io_error(step);
    }
    return make_io_result(C_IOResultError);
}

C_IOResult galay_ssl_socket_close(galay_ssl_socket_t* socket, int64_t timeout_ms)
{
    if (!valid_socket(socket)) {
        return make_io_result(C_IOResultInvalid);
    }
    return galay_kernel_tcp_socket_close(&socket->transport, timeout_ms);
}

galay_status_t galay_ssl_socket_get_protocol(const galay_ssl_socket_t* socket, char* out,
                                             size_t out_len, size_t* written)
{
    if (!valid_engine_socket(socket)) {
        return GALAY_INVALID_ARGUMENT;
    }
    return copy_string_status(socket->engine->getProtocolVersion(), out, out_len, written);
}

galay_status_t galay_ssl_socket_get_cipher(const galay_ssl_socket_t* socket, char* out,
                                           size_t out_len, size_t* written)
{
    if (!valid_engine_socket(socket)) {
        return GALAY_INVALID_ARGUMENT;
    }
    return copy_string_status(socket->engine->getCipher(), out, out_len, written);
}

galay_status_t galay_ssl_socket_get_negotiated_alpn(const galay_ssl_socket_t* socket, char* out,
                                                    size_t out_len, size_t* written)
{
    if (!valid_engine_socket(socket)) {
        return GALAY_INVALID_ARGUMENT;
    }
    return copy_string_status(socket->engine->getALPNProtocol(), out, out_len, written);
}

}
