/**
 * @file defn.hpp
 * @brief SSL/TLS 基础类型定义
 * @author galay-ssl
 * @version 1.0.0
 *
 * @details 定义 SSL/TLS 操作使用的基础枚举类型，包括协议方法、验证模式、
 * 握手状态、IO 结果和文件类型，以及错误码转换工具函数。
 */

#ifndef GALAY_SSL_DEFN_HPP
#define GALAY_SSL_DEFN_HPP

#include <cstdint>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/x509.h>

namespace galay::ssl
{

/**
 * @brief SSL/TLS 协议方法
 */
enum class SslMethod : uint8_t {
    TLS_Client,         ///< TLS 客户端（自动协商最高版本）
    TLS_Server,         ///< TLS 服务端（自动协商最高版本）
    TLS_1_2_Client,     ///< TLS 1.2 客户端
    TLS_1_2_Server,     ///< TLS 1.2 服务端
    TLS_1_3_Client,     ///< TLS 1.3 客户端
    TLS_1_3_Server,     ///< TLS 1.3 服务端
    DTLS_Client,        ///< DTLS 客户端
    DTLS_Server,        ///< DTLS 服务端
};

/**
 * @brief SSL 验证模式
 */
enum class SslVerifyMode : uint32_t {
    None            = SSL_VERIFY_NONE,                      ///< 不验证对端证书
    Peer            = SSL_VERIFY_PEER,                      ///< 验证对端证书
    FailIfNoPeerCert= SSL_VERIFY_FAIL_IF_NO_PEER_CERT,     ///< 对端无证书则失败
    ClientOnce      = SSL_VERIFY_CLIENT_ONCE,              ///< 仅验证客户端一次
};

/**
 * @brief SSL 握手状态
 */
enum class SslHandshakeState : uint8_t {
    NotStarted,     ///< 未开始
    InProgress,     ///< 进行中
    Completed,      ///< 已完成
    Failed,         ///< 失败
};

/**
 * @brief SSL IO 操作结果
 */
enum class SslIOResult : int {
    Success = 0,        ///< 成功
    WantRead = 1,       ///< 需要读取更多数据
    WantWrite = 2,      ///< 需要写入更多数据
    Error = -1,         ///< 错误
    ZeroReturn = -2,    ///< 对端关闭连接
    Syscall = -3,       ///< 系统调用错误
};

/**
 * @brief 将 SSL_get_error 结果转换为 SslIOResult
 */
inline SslIOResult sslErrorToResult(int ssl_error) {
    switch (ssl_error) {
        case SSL_ERROR_NONE:
            return SslIOResult::Success;
        case SSL_ERROR_WANT_READ:
            return SslIOResult::WantRead;
        case SSL_ERROR_WANT_WRITE:
            return SslIOResult::WantWrite;
        case SSL_ERROR_ZERO_RETURN:
            return SslIOResult::ZeroReturn;
        case SSL_ERROR_SYSCALL:
            return SslIOResult::Syscall;
        default:
            return SslIOResult::Error;
    }
}

/**
 * @brief SSL 文件类型
 */
enum class SslFileType : int {
    PEM = SSL_FILETYPE_PEM,     ///< PEM 格式
    ASN1 = SSL_FILETYPE_ASN1,   ///< ASN.1/DER 格式
};

} // namespace galay::ssl

#endif // GALAY_SSL_DEFN_HPP
