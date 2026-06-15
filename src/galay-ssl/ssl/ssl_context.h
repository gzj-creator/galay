/**
 * @file ssl_context.h
 * @brief SSL 上下文管理
 * @author galay-ssl
 * @version 1.0.0
 *
 * @details 封装 OpenSSL SSL_CTX，提供证书加载、验证模式配置、
 * 密码套件设置和 ALPN 协议协商等 SSL 上下文管理功能。
 * 一个 SSL 上下文可被多个 SSL 连接共享。
 */

#ifndef GALAY_SSL_CONTEXT_H
#define GALAY_SSL_CONTEXT_H

#include "galay-ssl/common/defn.hpp"
#include "galay-ssl/common/error.h"
#include <expected>
#include <string>
#include <memory>
#include <functional>
#include <vector>

namespace galay::ssl
{

/**
 * @brief SSL 上下文类
 *
 * @details 封装 OpenSSL SSL_CTX，管理 SSL 配置和证书。
 * 一个 SSL 上下文可以被多个 SSL 连接共享。
 *
 * @example
 * @code
 * // 服务端上下文
 * SslContext serverCtx(SslMethod::TLS_Server);
 * serverCtx.loadCertificate("server.crt");
 * serverCtx.loadPrivateKey("server.key");
 *
 * // 客户端上下文
 * SslContext clientCtx(SslMethod::TLS_Client);
 * clientCtx.setVerifyMode(SslVerifyMode::Peer);
 * clientCtx.loadCACertificate("ca.crt");
 * @endcode
 *
 * @note
 * - 不可拷贝，仅支持移动语义
 * - 线程安全：SSL_CTX 本身是线程安全的
 */
class SslContext
{
public:
    /**
     * @brief 构造 SSL 上下文
     * @param method SSL/TLS 协议方法
     */
    explicit SslContext(SslMethod method);

    /**
     * @brief 析构函数
     */
    ~SslContext();

    /// @brief 禁用拷贝
    SslContext(const SslContext&) = delete;
    SslContext& operator=(const SslContext&) = delete;

    /**
     * @brief 移动构造
     */
    SslContext(SslContext&& other) noexcept;

    /**
     * @brief 移动赋值
     */
    SslContext& operator=(SslContext&& other) noexcept;

    /**
     * @brief 检查上下文是否有效
     */
    bool isValid() const { return m_ctx != nullptr; }

    /**
     * @brief 获取底层 SSL_CTX 指针
     */
    SSL_CTX* native() const { return m_ctx; }

    /**
     * @brief 加载证书文件
     *
     * @param certFile 证书文件路径
     * @param type 文件类型，默认 PEM
     * @return 成功返回 void，失败返回 SslError
     */
    std::expected<void, SslError> loadCertificate(
        const std::string& certFile,
        SslFileType type = SslFileType::PEM);

    /**
     * @brief 加载证书链文件
     *
     * @param certChainFile 证书链文件路径
     * @return 成功返回 void，失败返回 SslError
     */
    std::expected<void, SslError> loadCertificateChain(const std::string& certChainFile);

    /**
     * @brief 加载私钥文件
     *
     * @param keyFile 私钥文件路径
     * @param type 文件类型，默认 PEM
     * @return 成功返回 void，失败返回 SslError
     */
    std::expected<void, SslError> loadPrivateKey(
        const std::string& keyFile,
        SslFileType type = SslFileType::PEM);

    /**
     * @brief 加载 CA 证书文件
     *
     * @param caFile CA 证书文件路径
     * @return 成功返回 void，失败返回 SslError
     */
    std::expected<void, SslError> loadCACertificate(const std::string& caFile);

    /**
     * @brief 加载 CA 证书目录
     *
     * @param caPath CA 证书目录路径
     * @return 成功返回 void，失败返回 SslError
     */
    std::expected<void, SslError> loadCAPath(const std::string& caPath);

    /**
     * @brief 使用系统默认 CA 证书
     * @return 成功返回 void，失败返回 SslError
     */
    std::expected<void, SslError> useDefaultCA();

    /**
     * @brief 设置验证模式
     *
     * @param mode 验证模式
     * @param callback 可选的验证回调函数
     */
    void setVerifyMode(SslVerifyMode mode,
                       std::function<bool(bool, X509_STORE_CTX*)> callback = nullptr);

    /**
     * @brief 设置验证深度
     * @param depth 证书链验证深度
     */
    void setVerifyDepth(int depth);

    /**
     * @brief 设置密码套件（TLS 1.2 及以下）
     *
     * @param ciphers 密码套件字符串
     * @return 成功返回 void，失败返回 SslError
     */
    std::expected<void, SslError> setCiphers(const std::string& ciphers);

    /**
     * @brief 设置密码套件（TLS 1.3）
     *
     * @param ciphersuites TLS 1.3 密码套件字符串
     * @return 成功返回 void，失败返回 SslError
     */
    std::expected<void, SslError> setCiphersuites(const std::string& ciphersuites);

    /**
     * @brief 设置 ALPN 协议列表
     *
     * @param protocols 协议列表，如 {"h2", "http/1.1"}
     * @return 成功返回 void，失败返回 SslError
     */
    std::expected<void, SslError> setALPNProtocols(const std::vector<std::string>& protocols);

    /**
     * @brief 设置服务端 ALPN 选择列表
     *
     * @param protocols 按优先级排列的协议列表，如 {"h2", "http/1.1"}
     * @return 成功返回 void，失败返回 SslError
     */
    std::expected<void, SslError> setALPNSelectProtocols(const std::vector<std::string>& protocols);

    /**
     * @brief 设置最小 TLS 版本
     * @param version TLS 版本（如 TLS1_2_VERSION）
     */
    void setMinProtocolVersion(int version);

    /**
     * @brief 设置最大 TLS 版本
     * @param version TLS 版本（如 TLS1_3_VERSION）
     */
    void setMaxProtocolVersion(int version);

    /**
     * @brief 启用会话缓存
     * @param mode 缓存模式
     */
    void setSessionCacheMode(long mode);

    /**
     * @brief 设置会话超时时间
     * @param timeout 超时秒数
     */
    void setSessionTimeout(long timeout);

    /**
     * @brief 关闭 SSL 会话缓存
     */
    void disableSessionCache();

    /**
     * @brief 关闭 TLS session ticket
     */
    void disableSessionTickets();

    /**
     * @brief 获取创建时的错误
     */
    const SslError& error() const { return m_error; }

private:
    static int selectALPNCallback(SSL* ssl,
                                  const unsigned char** out,
                                  unsigned char* outlen,
                                  const unsigned char* in,
                                  unsigned int inlen,
                                  void* arg);
    void refreshCallbackContext() noexcept;

    SSL_CTX* m_ctx;                                             ///< OpenSSL SSL_CTX
    SslError m_error;                                           ///< 创建时的错误
    std::function<bool(bool, X509_STORE_CTX*)> m_verifyCallback;///< 验证回调
    std::vector<std::string> m_alpnSelectProtocols;             ///< 服务端 ALPN 选择优先级
};

} // namespace galay::ssl

#endif // GALAY_SSL_CONTEXT_H
