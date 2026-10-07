#include "ssl_context.h"
#include "../common/ssl_log.h"
#include <algorithm>
#include <cstring>
#include <mutex>

namespace galay::ssl
{

namespace {

std::once_flag g_ssl_init_once;

void initialize_open_ssl() {
    std::call_once(g_ssl_init_once, [] {
        SSL_library_init();
        SSL_load_error_strings();
        OpenSSL_add_all_algorithms();
    });
}

const SSL_METHOD* get_method(SslMethod method) {
    switch (method) {
        case SslMethod::TLS_Client:
            return TLS_client_method();
        case SslMethod::TLS_Server:
            return TLS_server_method();
        case SslMethod::TLS_1_2_Client:
        case SslMethod::TLS_1_2_Server:
            return TLS_method();
        case SslMethod::TLS_1_3_Client:
        case SslMethod::TLS_1_3_Server:
            return TLS_method();
        case SslMethod::DTLS_Client:
            return DTLS_client_method();
        case SslMethod::DTLS_Server:
            return DTLS_server_method();
        default:
            return TLS_method();
    }
}

bool is_server_method(SslMethod method) {
    switch (method) {
        case SslMethod::TLS_Server:
        case SslMethod::TLS_1_2_Server:
        case SslMethod::TLS_1_3_Server:
        case SslMethod::DTLS_Server:
            return true;
        default:
            return false;
    }
}

} // anonymous namespace

SslContext::SslContext(SslMethod method)
    : m_ctx(nullptr)
{
    initialize_open_ssl();

    m_ctx = SSL_CTX_new(get_method(method));
    if (!m_ctx) {
        SSL_LOG_ERROR("[context] [create]", "SSL_CTX_new failed");
        m_error = SslError::from_open_ssl(SslErrorCode::kContextCreateFailed);
        return;
    }

    // 设置默认选项
    SSL_CTX_set_options(m_ctx, SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3);

    // 启用 Session 缓存以提高性能（减少完整握手次数）
    SSL_CTX_set_session_cache_mode(m_ctx, SSL_SESS_CACHE_BOTH);
    SSL_CTX_set_timeout(m_ctx, 300);  // Session 有效期 5 分钟

    // Default server contexts prioritize steady-state throughput and avoid
    // post-handshake TLS 1.3 session ticket traffic on long-lived connections.
    if (is_server_method(method)) {
        SSL_CTX_set_options(m_ctx, SSL_OP_NO_TICKET);
        SSL_CTX_set_num_tickets(m_ctx, 0);
    }

    // 根据方法设置版本限制
    switch (method) {
        case SslMethod::TLS_1_2_Client:
        case SslMethod::TLS_1_2_Server:
            SSL_CTX_set_min_proto_version(m_ctx, TLS1_2_VERSION);
            SSL_CTX_set_max_proto_version(m_ctx, TLS1_2_VERSION);
            break;
        case SslMethod::TLS_1_3_Client:
        case SslMethod::TLS_1_3_Server:
            SSL_CTX_set_min_proto_version(m_ctx, TLS1_3_VERSION);
            SSL_CTX_set_max_proto_version(m_ctx, TLS1_3_VERSION);
            break;
        default:
            // 使用默认版本范围
            break;
    }
}

SslContext::~SslContext()
{
    if (m_ctx) {
        SSL_CTX_free(m_ctx);
        m_ctx = nullptr;
    }
}

SslContext::SslContext(SslContext&& other) noexcept
    : m_ctx(other.m_ctx)
    , m_error(std::move(other.m_error))
    , m_verify_callback(std::move(other.m_verify_callback))
    , m_alpnSelectProtocols(std::move(other.m_alpnSelectProtocols))
{
    other.m_ctx = nullptr;
    refresh_callback_context();
}

SslContext& SslContext::operator=(SslContext&& other) noexcept
{
    if (this != &other) {
        if (m_ctx) {
            SSL_CTX_free(m_ctx);
        }
        m_ctx = other.m_ctx;
        m_error = std::move(other.m_error);
        m_verify_callback = std::move(other.m_verify_callback);
        m_alpnSelectProtocols = std::move(other.m_alpnSelectProtocols);
        other.m_ctx = nullptr;
        refresh_callback_context();
    }
    return *this;
}

std::expected<void, SslError> SslContext::load_certificate(
    const std::string& certFile,
    SslFileType type)
{
    if (!m_ctx) {
        return std::unexpected(SslError(SslErrorCode::kContextCreateFailed));
    }

    if (SSL_CTX_use_certificate_file(m_ctx, certFile.c_str(), static_cast<int>(type)) != 1) {
        unsigned long _cert_err = ERR_get_error();
        SSL_LOG_ERROR("[context] [cert]", "path={} error={}", certFile, ERR_error_string(_cert_err, nullptr));
        return std::unexpected(SslError(SslErrorCode::kCertificateLoadFailed, _cert_err));
    }

    return {};
}

std::expected<void, SslError> SslContext::load_certificate_chain(const std::string& certChainFile)
{
    if (!m_ctx) {
        return std::unexpected(SslError(SslErrorCode::kContextCreateFailed));
    }

    if (SSL_CTX_use_certificate_chain_file(m_ctx, certChainFile.c_str()) != 1) {
        return std::unexpected(SslError::from_open_ssl(SslErrorCode::kCertificateLoadFailed));
    }

    return {};
}

std::expected<void, SslError> SslContext::load_private_key(
    const std::string& keyFile,
    SslFileType type)
{
    if (!m_ctx) {
        return std::unexpected(SslError(SslErrorCode::kContextCreateFailed));
    }

    if (SSL_CTX_use_PrivateKey_file(m_ctx, keyFile.c_str(), static_cast<int>(type)) != 1) {
        unsigned long _key_err = ERR_get_error();
        SSL_LOG_ERROR("[context] [key]", "path={} error={}", keyFile, ERR_error_string(_key_err, nullptr));
        return std::unexpected(SslError(SslErrorCode::kPrivateKeyLoadFailed, _key_err));
    }

    // 验证私钥与证书匹配
    if (SSL_CTX_check_private_key(m_ctx) != 1) {
        unsigned long _mismatch_err = ERR_get_error();
        SSL_LOG_ERROR("[context] [key]", "path={} mismatch error={}", keyFile, ERR_error_string(_mismatch_err, nullptr));
        return std::unexpected(SslError(SslErrorCode::kPrivateKeyMismatch, _mismatch_err));
    }

    return {};
}

std::expected<void, SslError> SslContext::load_ca_certificate(const std::string& caFile)
{
    if (!m_ctx) {
        return std::unexpected(SslError(SslErrorCode::kContextCreateFailed));
    }

    if (SSL_CTX_load_verify_locations(m_ctx, caFile.c_str(), nullptr) != 1) {
        SSL_LOG_ERROR("[context] [ca]", "path={}", caFile);
        return std::unexpected(SslError::from_open_ssl(SslErrorCode::kCACertificateLoadFailed));
    }

    return {};
}

std::expected<void, SslError> SslContext::load_ca_path(const std::string& ca_path)
{
    if (!m_ctx) {
        return std::unexpected(SslError(SslErrorCode::kContextCreateFailed));
    }

    if (SSL_CTX_load_verify_locations(m_ctx, nullptr, ca_path.c_str()) != 1) {
        return std::unexpected(SslError::from_open_ssl(SslErrorCode::kCACertificateLoadFailed));
    }

    return {};
}

std::expected<void, SslError> SslContext::use_default_ca()
{
    if (!m_ctx) {
        return std::unexpected(SslError(SslErrorCode::kContextCreateFailed));
    }

    if (SSL_CTX_set_default_verify_paths(m_ctx) != 1) {
        return std::unexpected(SslError::from_open_ssl(SslErrorCode::kCACertificateLoadFailed));
    }

    return {};
}

void SslContext::set_verify_mode(SslVerifyMode mode,
                                std::function<bool(bool, X509_STORE_CTX*)> callback)
{
    if (!m_ctx) return;

    m_verify_callback = std::move(callback);

    if (m_verify_callback) {
        // 设置带回调的验证
        SSL_CTX_set_verify(m_ctx, static_cast<int>(mode),
            [](int preverify_ok, X509_STORE_CTX* ctx) -> int {
                // 获取 SSL 对象
                SSL* ssl = static_cast<SSL*>(X509_STORE_CTX_get_ex_data(
                    ctx, SSL_get_ex_data_X509_STORE_CTX_idx()));
                if (!ssl) return preverify_ok;

                // 获取 SSL_CTX
                SSL_CTX* ssl_ctx = SSL_get_SSL_CTX(ssl);
                if (!ssl_ctx) return preverify_ok;

                // 获取 SslContext 指针
                SslContext* self = static_cast<SslContext*>(SSL_CTX_get_ex_data(ssl_ctx, 0));
                if (!self || !self->m_verify_callback) return preverify_ok;

                if (preverify_ok == 0) {
                    int depth = X509_STORE_CTX_get_error_depth(ctx);
                    int err = X509_STORE_CTX_get_error(ctx);
                    X509* cert = X509_STORE_CTX_get_current_cert(ctx);
                    char subject[256] = {};
                    if (cert) {
                        X509_NAME_oneline(X509_get_subject_name(cert), subject, sizeof(subject));
                    }
                    SSL_LOG_WARN("[verify]", "depth={} err={} subject={}", depth, err, subject);
                }

                return self->m_verify_callback(preverify_ok != 0, ctx) ? 1 : 0;
            });

        // 存储 this 指针
        SSL_CTX_set_ex_data(m_ctx, 0, this);
    } else {
        SSL_CTX_set_verify(m_ctx, static_cast<int>(mode), nullptr);
    }
}

void SslContext::set_verify_depth(int depth)
{
    if (m_ctx) {
        SSL_CTX_set_verify_depth(m_ctx, depth);
    }
}

std::expected<void, SslError> SslContext::set_ciphers(const std::string& ciphers)
{
    if (!m_ctx) {
        return std::unexpected(SslError(SslErrorCode::kContextCreateFailed));
    }

    if (SSL_CTX_set_cipher_list(m_ctx, ciphers.c_str()) != 1) {
        return std::unexpected(SslError::from_open_ssl(SslErrorCode::kUnknown));
    }

    return {};
}

std::expected<void, SslError> SslContext::set_ciphersuites(const std::string& ciphersuites)
{
    if (!m_ctx) {
        return std::unexpected(SslError(SslErrorCode::kContextCreateFailed));
    }

    if (SSL_CTX_set_ciphersuites(m_ctx, ciphersuites.c_str()) != 1) {
        return std::unexpected(SslError::from_open_ssl(SslErrorCode::kUnknown));
    }

    return {};
}

std::expected<void, SslError> SslContext::set_alpn_protocols(const std::vector<std::string>& protocols)
{
    if (!m_ctx) {
        return std::unexpected(SslError(SslErrorCode::kContextCreateFailed));
    }

    // 构建 ALPN 协议字符串（长度前缀格式）
    std::vector<unsigned char> alpn;
    for (const auto& proto : protocols) {
        if (proto.size() > 255) continue;
        alpn.push_back(static_cast<unsigned char>(proto.size()));
        alpn.insert(alpn.end(), proto.begin(), proto.end());
    }

    if (SSL_CTX_set_alpn_protos(m_ctx, alpn.data(), static_cast<unsigned int>(alpn.size())) != 0) {
        return std::unexpected(SslError(SslErrorCode::kALPNSetFailed));
    }

    return {};
}

std::expected<void, SslError> SslContext::set_alpn_select_protocols(const std::vector<std::string>& protocols)
{
    if (!m_ctx) {
        return std::unexpected(SslError(SslErrorCode::kContextCreateFailed));
    }

    m_alpnSelectProtocols.clear();
    for (const auto& proto : protocols) {
        if (!proto.empty() && proto.size() <= 255) {
            m_alpnSelectProtocols.push_back(proto);
        }
    }
    if (m_alpnSelectProtocols.empty()) {
        return std::unexpected(SslError(SslErrorCode::kALPNSetFailed));
    }

    SSL_CTX_set_alpn_select_cb(m_ctx, &SslContext::select_alpn_callback, this);
    return {};
}

int SslContext::select_alpn_callback(SSL*,
                                   const unsigned char** out,
                                   unsigned char* outlen,
                                   const unsigned char* in,
                                   unsigned int inlen,
                                   void* arg)
{
    auto* self = static_cast<SslContext*>(arg);
    if (!self || !out || !outlen || !in) {
        return SSL_TLSEXT_ERR_NOACK;
    }

    for (const auto& preferred : self->m_alpnSelectProtocols) {
        unsigned int offset = 0;
        while (offset < inlen) {
            const unsigned int len = in[offset++];
            if (offset + len > inlen) {
                return SSL_TLSEXT_ERR_NOACK;
            }
            if (len == preferred.size() &&
                std::equal(preferred.begin(), preferred.end(), in + offset)) {
                *out = in + offset;
                *outlen = static_cast<unsigned char>(len);
                return SSL_TLSEXT_ERR_OK;
            }
            offset += len;
        }
    }

    return SSL_TLSEXT_ERR_NOACK;
}

void SslContext::refresh_callback_context() noexcept
{
    if (!m_ctx) {
        return;
    }
    if (m_verify_callback) {
        SSL_CTX_set_ex_data(m_ctx, 0, this);
    }
    if (!m_alpnSelectProtocols.empty()) {
        SSL_CTX_set_alpn_select_cb(m_ctx, &SslContext::select_alpn_callback, this);
    }
}

void SslContext::set_min_protocol_version(int version)
{
    if (m_ctx) {
        SSL_CTX_set_min_proto_version(m_ctx, version);
    }
}

void SslContext::set_max_protocol_version(int version)
{
    if (m_ctx) {
        SSL_CTX_set_max_proto_version(m_ctx, version);
    }
}

void SslContext::set_session_cache_mode(long mode)
{
    if (m_ctx) {
        SSL_CTX_set_session_cache_mode(m_ctx, mode);
    }
}

void SslContext::set_session_timeout(long timeout)
{
    if (m_ctx) {
        SSL_CTX_set_timeout(m_ctx, timeout);
    }
}

void SslContext::disable_session_cache()
{
    if (m_ctx) {
        SSL_CTX_set_session_cache_mode(m_ctx, SSL_SESS_CACHE_OFF);
    }
}

void SslContext::disable_session_tickets()
{
    if (m_ctx) {
        SSL_CTX_set_options(m_ctx, SSL_OP_NO_TICKET);
        SSL_CTX_set_num_tickets(m_ctx, 0);
    }
}

} // namespace galay::ssl
