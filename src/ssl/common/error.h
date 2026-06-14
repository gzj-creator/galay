/**
 * @file error.h
 * @brief SSL 错误码与错误报告
 * @author galay-ssl
 * @version 1.0.0
 *
 * @details 定义 SSL/TLS 操作的错误码枚举 SslErrorCode 和错误封装类 SslError，
 * 提供统一的 SSL 错误处理机制，集成 OpenSSL 错误队列信息。
 */

#ifndef GALAY_SSL_ERROR_H
#define GALAY_SSL_ERROR_H

#include <cstdint>
#include <string>
#include <openssl/err.h>

namespace galay::ssl
{

/**
 * @brief SSL 错误码
 */
enum class SslErrorCode : uint32_t {
    kSuccess = 0,               ///< 成功
    kContextCreateFailed,       ///< SSL 上下文创建失败
    kCertificateLoadFailed,     ///< 证书加载失败
    kPrivateKeyLoadFailed,      ///< 私钥加载失败
    kPrivateKeyMismatch,        ///< 私钥与证书不匹配
    kCACertificateLoadFailed,   ///< CA 证书加载失败
    kSslCreateFailed,           ///< SSL 对象创建失败
    kSslSetFdFailed,            ///< 设置 SSL fd 失败
    kHandshakeFailed,           ///< 握手失败
    kHandshakeTimeout,          ///< 握手超时
    kHandshakeWantRead,         ///< 握手需要读取
    kHandshakeWantWrite,        ///< 握手需要写入
    kReadFailed,                ///< 读取失败
    kWriteFailed,               ///< 写入失败
    kShutdownFailed,            ///< 关闭失败
    kPeerClosed,                ///< 对端关闭
    kVerificationFailed,        ///< 证书验证失败
    kSNISetFailed,              ///< SNI 设置失败
    kALPNSetFailed,             ///< ALPN 设置失败
    kTimeout,                   ///< 操作超时
    kUnknown,                   ///< 未知错误
};

/**
 * @brief SSL 错误类
 * @details 封装 SSL 错误码和 OpenSSL 错误信息
 */
class SslError
{
public:
    /**
     * @brief 构造成功状态
     */
    SslError() : m_code(SslErrorCode::kSuccess), m_ssl_error(0) {}

    /**
     * @brief 构造错误对象
     * @param code SSL 错误码
     * @param ssl_error OpenSSL 错误码（可选）
     */
    explicit SslError(SslErrorCode code, unsigned long ssl_error = 0)
        : m_code(code), m_ssl_error(ssl_error) {
        if (ssl_error == 0) {
            m_ssl_error = ERR_peek_last_error();
        }
    }

    /**
     * @brief 从当前 OpenSSL 错误队列创建错误对象
     * @param code SSL 错误码
     * @return SslError 对象
     */
    static SslError fromOpenSSL(SslErrorCode code) {
        return SslError(code, ERR_get_error());
    }

    /**
     * @brief 检查是否成功
     */
    bool isSuccess() const { return m_code == SslErrorCode::kSuccess; }

    /**
     * @brief 检查是否需要重试（WANT_READ/WANT_WRITE）
     */
    bool needsRetry() const {
        return m_code == SslErrorCode::kHandshakeWantRead ||
               m_code == SslErrorCode::kHandshakeWantWrite;
    }

    /**
     * @brief 获取错误码
     */
    SslErrorCode code() const { return m_code; }

    /**
     * @brief 获取 OpenSSL 错误码
     */
    unsigned long sslError() const { return m_ssl_error; }

    /**
     * @brief 获取错误消息
     */
    std::string message() const;

    /**
     * @brief 获取 OpenSSL 错误字符串
     */
    std::string sslErrorString() const;

private:
    SslErrorCode m_code;        ///< SSL 错误码
    unsigned long m_ssl_error;  ///< OpenSSL 错误码
};

} // namespace galay::ssl

#endif // GALAY_SSL_ERROR_H
