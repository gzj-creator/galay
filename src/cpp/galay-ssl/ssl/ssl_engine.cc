#include "ssl_engine.h"
#include <galay/cpp/galay-ssl/common/ssl_log.h>

namespace galay::ssl
{

namespace {

void clearOpenSslErrorQueueIfNeeded()
{
    if (ERR_peek_error() != 0) {
        ERR_clear_error();
    }
}

} // namespace

SslEngine::SslEngine(SslContext* ctx)
    : m_ssl(nullptr)
    , m_ctx(ctx)
    , m_handshakeState(SslHandshakeState::NotStarted)
{
    if (ctx && ctx->isValid()) {
        m_ssl = SSL_new(ctx->native());
    }
    if (!m_ssl) {
        SSL_LOG_ERROR("[engine] [create]", "SSL_new failed");
    }
}

SslEngine::~SslEngine()
{
    if (m_ssl) {
        SSL_free(m_ssl);
        m_ssl = nullptr;
    }
}

SslEngine::SslEngine(SslEngine&& other) noexcept
    : m_ssl(other.m_ssl)
    , m_ctx(other.m_ctx)
    , m_rbio(other.m_rbio)
    , m_wbio(other.m_wbio)
    , m_handshakeState(other.m_handshakeState)
{
    other.m_ssl = nullptr;
    other.m_ctx = nullptr;
    other.m_handshakeState = SslHandshakeState::NotStarted;
    other.m_rbio = nullptr;
    other.m_wbio = nullptr;
}

SslEngine& SslEngine::operator=(SslEngine&& other) noexcept
{
    if (this != &other) {
        if (m_ssl) {
            SSL_free(m_ssl);
        }
        m_ssl = other.m_ssl;
        m_ctx = other.m_ctx;
        m_handshakeState = other.m_handshakeState;
        m_rbio = other.m_rbio;
        m_wbio = other.m_wbio;
        other.m_ssl = nullptr;
        other.m_ctx = nullptr;
        other.m_handshakeState = SslHandshakeState::NotStarted;
        other.m_rbio = nullptr;
        other.m_wbio = nullptr;
    }
    return *this;
}

std::expected<void, SslError> SslEngine::setFd(int fd)
{
    if (!m_ssl) {
        return std::unexpected(SslError(SslErrorCode::kSslCreateFailed));
    }

    if (SSL_set_fd(m_ssl, fd) != 1) {
        return std::unexpected(SslError::fromOpenSSL(SslErrorCode::kSslSetFdFailed));
    }

    return {};
}

std::expected<void, SslError> SslEngine::initMemoryBIO()
{
    if (!m_ssl) {
        return std::unexpected(SslError(SslErrorCode::kSslCreateFailed));
    }
    if (m_rbio && m_wbio) {
        return {};
    }
    if (m_rbio || m_wbio) {
        return std::unexpected(SslError(SslErrorCode::kSslCreateFailed));
    }

    m_rbio = BIO_new(BIO_s_mem());
    m_wbio = BIO_new(BIO_s_mem());
    if (!m_rbio || !m_wbio) {
        SSL_LOG_ERROR("[engine] [bio]", "BIO_new failed");
        if (m_rbio) BIO_free(m_rbio);
        if (m_wbio) BIO_free(m_wbio);
        m_rbio = nullptr;
        m_wbio = nullptr;
        return std::unexpected(SslError(SslErrorCode::kSslCreateFailed));
    }

    // SSL_set_bio 接管 BIO 生命周期，SSL_free 时自动释放
    SSL_set_bio(m_ssl, m_rbio, m_wbio);
    return {};
}

int SslEngine::feedEncryptedInput(const char* data, size_t length)
{
    if (!m_rbio) return -1;
    return BIO_write(m_rbio, data, static_cast<int>(length));
}

int SslEngine::extractEncryptedOutput(char* buffer, size_t length)
{
    if (!m_wbio) return -1;
    return BIO_read(m_wbio, buffer, static_cast<int>(length));
}

size_t SslEngine::pendingEncryptedOutput() const
{
    if (!m_wbio) return 0;
    return BIO_ctrl_pending(m_wbio);
}

std::expected<void, SslError> SslEngine::setHostname(const std::string& hostname)
{
    if (!m_ssl) {
        return std::unexpected(SslError(SslErrorCode::kSslCreateFailed));
    }

    // 设置 SNI
    if (SSL_set_tlsext_host_name(m_ssl, hostname.c_str()) != 1) {
        return std::unexpected(SslError::fromOpenSSL(SslErrorCode::kSNISetFailed));
    }

    // 设置主机名验证
    if (SSL_set1_host(m_ssl, hostname.c_str()) != 1) {
        return std::unexpected(SslError::fromOpenSSL(SslErrorCode::kSNISetFailed));
    }

    return {};
}

void SslEngine::setConnectState()
{
    if (m_ssl) {
        SSL_set_connect_state(m_ssl);
    }
}

void SslEngine::setAcceptState()
{
    if (m_ssl) {
        SSL_set_accept_state(m_ssl);
    }
}

SslIOResult SslEngine::doHandshake()
{
    if (!m_ssl) {
        return SslIOResult::Error;
    }

    m_handshakeState = SslHandshakeState::InProgress;

    int ret = SSL_do_handshake(m_ssl);
    if (ret == 1) {
        m_handshakeState = SslHandshakeState::Completed;
        SSL_LOG_INFO("[handshake] [ok]", "protocol={} cipher={}", getProtocolVersion(), getCipher());
        return SslIOResult::Success;
    }

    int err = SSL_get_error(m_ssl, ret);
    SslIOResult result = sslErrorToResult(err);

    if (result == SslIOResult::Error ||
        result == SslIOResult::Syscall ||
        result == SslIOResult::ZeroReturn) {
        m_handshakeState = SslHandshakeState::Failed;
        SSL_LOG_ERROR("[handshake] [fail]", "ssl_error={}", static_cast<int>(result));
    }

    return result;
}

SslIOResult SslEngine::read(char* buffer, size_t length, size_t& bytesRead)
{
    if (!m_ssl) {
        return SslIOResult::Error;
    }

    bytesRead = 0;
    clearOpenSslErrorQueueIfNeeded();
    int ret = SSL_read(m_ssl, buffer, static_cast<int>(length));

    if (ret > 0) {
        bytesRead = static_cast<size_t>(ret);
        return SslIOResult::Success;
    }

    int err = SSL_get_error(m_ssl, ret);

    // 当 ret == 0 且 err == SSL_ERROR_SYSCALL 且 errno == 0 时表示 EOF
    if (ret == 0 && err == SSL_ERROR_SYSCALL && errno == 0) {
        return SslIOResult::ZeroReturn;
    }

    // 当 ret == 0 且 err == SSL_ERROR_ZERO_RETURN 时表示对端正常关闭
    if (ret == 0 && err == SSL_ERROR_ZERO_RETURN) {
        return SslIOResult::ZeroReturn;
    }

    return sslErrorToResult(err);
}

SslIOResult SslEngine::write(const char* buffer, size_t length, size_t& bytesWritten)
{
    if (!m_ssl) {
        return SslIOResult::Error;
    }

    bytesWritten = 0;
    clearOpenSslErrorQueueIfNeeded();
    int ret = SSL_write(m_ssl, buffer, static_cast<int>(length));

    if (ret > 0) {
        bytesWritten = static_cast<size_t>(ret);
        return SslIOResult::Success;
    }

    return sslErrorToResult(SSL_get_error(m_ssl, ret));
}

SslIOResult SslEngine::shutdown()
{
    if (!m_ssl) {
        return SslIOResult::Success;
    }

    int ret = SSL_shutdown(m_ssl);

    if (ret == 1) {
        // 完全关闭
        return SslIOResult::Success;
    } else if (ret == 0) {
        // 需要再次调用
        return SslIOResult::WantRead;
    }

    return sslErrorToResult(SSL_get_error(m_ssl, ret));
}

X509* SslEngine::getPeerCertificate() const
{
    if (!m_ssl) {
        return nullptr;
    }
    return SSL_get_peer_certificate(m_ssl);
}

long SslEngine::getVerifyResult() const
{
    if (!m_ssl) {
        return X509_V_ERR_APPLICATION_VERIFICATION;
    }
    return SSL_get_verify_result(m_ssl);
}

std::string SslEngine::getProtocolVersion() const
{
    if (!m_ssl) {
        return "";
    }
    return SSL_get_version(m_ssl);
}

std::string SslEngine::getCipher() const
{
    if (!m_ssl) {
        return "";
    }
    const SSL_CIPHER* cipher = SSL_get_current_cipher(m_ssl);
    if (!cipher) {
        return "";
    }
    return SSL_CIPHER_get_name(cipher);
}

std::string SslEngine::getALPNProtocol() const
{
    if (!m_ssl) {
        return "";
    }

    const unsigned char* data = nullptr;
    unsigned int len = 0;
    SSL_get0_alpn_selected(m_ssl, &data, &len);

    if (data && len > 0) {
        return std::string(reinterpret_cast<const char*>(data), len);
    }
    return "";
}

int SslEngine::getError(int ret) const
{
    if (!m_ssl) {
        return SSL_ERROR_SSL;
    }
    return SSL_get_error(m_ssl, ret);
}

size_t SslEngine::pending() const
{
    if (!m_ssl) {
        return 0;
    }
    return static_cast<size_t>(SSL_pending(m_ssl));
}

bool SslEngine::setSession(SSL_SESSION* session)
{
    if (!m_ssl || !session) {
        return false;
    }
    return SSL_set_session(m_ssl, session) == 1;
}

SSL_SESSION* SslEngine::getSession() const
{
    if (!m_ssl) {
        return nullptr;
    }
    return SSL_get1_session(m_ssl);
}

bool SslEngine::isSessionReused() const
{
    if (!m_ssl) {
        return false;
    }
    return SSL_session_reused(m_ssl) == 1;
}

} // namespace galay::ssl
