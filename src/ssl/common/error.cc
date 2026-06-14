#include "error.h"
#include <sstream>

namespace galay::ssl
{

std::string SslError::message() const
{
    std::ostringstream oss;

    switch (m_code) {
        case SslErrorCode::kSuccess:
            return "Success";
        case SslErrorCode::kContextCreateFailed:
            oss << "Failed to create SSL context";
            break;
        case SslErrorCode::kCertificateLoadFailed:
            oss << "Failed to load certificate";
            break;
        case SslErrorCode::kPrivateKeyLoadFailed:
            oss << "Failed to load private key";
            break;
        case SslErrorCode::kPrivateKeyMismatch:
            oss << "Private key does not match certificate";
            break;
        case SslErrorCode::kCACertificateLoadFailed:
            oss << "Failed to load CA certificate";
            break;
        case SslErrorCode::kSslCreateFailed:
            oss << "Failed to create SSL object";
            break;
        case SslErrorCode::kSslSetFdFailed:
            oss << "Failed to set SSL file descriptor";
            break;
        case SslErrorCode::kHandshakeFailed:
            oss << "SSL handshake failed";
            break;
        case SslErrorCode::kHandshakeTimeout:
            oss << "SSL handshake timed out";
            break;
        case SslErrorCode::kHandshakeWantRead:
            oss << "SSL handshake wants read";
            break;
        case SslErrorCode::kHandshakeWantWrite:
            oss << "SSL handshake wants write";
            break;
        case SslErrorCode::kReadFailed:
            oss << "SSL read failed";
            break;
        case SslErrorCode::kWriteFailed:
            oss << "SSL write failed";
            break;
        case SslErrorCode::kShutdownFailed:
            oss << "SSL shutdown failed";
            break;
        case SslErrorCode::kPeerClosed:
            oss << "Peer closed connection";
            break;
        case SslErrorCode::kVerificationFailed:
            oss << "Certificate verification failed";
            break;
        case SslErrorCode::kSNISetFailed:
            oss << "Failed to set SNI hostname";
            break;
        case SslErrorCode::kALPNSetFailed:
            oss << "Failed to set ALPN protocols";
            break;
        case SslErrorCode::kTimeout:
            oss << "Operation timed out";
            break;
        case SslErrorCode::kUnknown:
        default:
            oss << "Unknown SSL error";
            break;
    }

    if (m_ssl_error != 0) {
        oss << ": " << sslErrorString();
    }

    return oss.str();
}

std::string SslError::sslErrorString() const
{
    if (m_ssl_error == 0) {
        return "";
    }

    char buf[256];
    ERR_error_string_n(m_ssl_error, buf, sizeof(buf));
    return std::string(buf);
}

} // namespace galay::ssl
