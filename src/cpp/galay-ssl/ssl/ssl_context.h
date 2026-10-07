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

#include "../common/defn.hpp"
#include "../common/error.h"
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
 * SslContext server_ctx(SslMethod::TLS_Server);
 * server_ctx.load_certificate("server.crt");
 * server_ctx.load_private_key("server.key");
 *
 * // 客户端上下文
 * SslContext client_ctx(SslMethod::TLS_Client);
 * client_ctx.set_verify_mode(SslVerifyMode::Peer);
 * client_ctx.load_ca_certificate("ca.crt");
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
    bool is_valid() const { return m_ctx != nullptr; }

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
    std::expected<void, SslError> load_certificate(
        const std::string& certFile,
        SslFileType type = SslFileType::PEM);

    /**
     * @brief 加载证书链文件
     *
     * @param certChainFile 证书链文件路径
     * @return 成功返回 void，失败返回 SslError
     */
    std::expected<void, SslError> load_certificate_chain(const std::string& certChainFile);

    /**
     * @brief 加载私钥文件
     *
     * @param keyFile 私钥文件路径
     * @param type 文件类型，默认 PEM
     * @return 成功返回 void，失败返回 SslError
     */
    std::expected<void, SslError> load_private_key(
        const std::string& keyFile,
        SslFileType type = SslFileType::PEM);

    /**
     * @brief 加载 CA 证书文件
     *
     * @param caFile CA 证书文件路径
     * @return 成功返回 void，失败返回 SslError
     */
    std::expected<void, SslError> load_ca_certificate(const std::string& caFile);

    /**
     * @brief 加载 CA 证书目录
     *
     * @param ca_path CA 证书目录路径
     * @return 成功返回 void，失败返回 SslError
     */
    std::expected<void, SslError> load_ca_path(const std::string& ca_path);

    /**
     * @brief 使用系统默认 CA 证书
     * @return 成功返回 void，失败返回 SslError
     */
    std::expected<void, SslError> use_default_ca();

    /**
     * @brief 设置验证模式
     *
     * @param mode 验证模式
     * @param callback 可选的验证回调函数
     */
    void set_verify_mode(SslVerifyMode mode,
                       std::function<bool(bool, X509_STORE_CTX*)> callback = nullptr);

    /**
     * @brief 设置验证深度
     * @param depth 证书链验证深度
     */
    void set_verify_depth(int depth);

    /**
     * @brief 设置密码套件（TLS 1.2 及以下）
     *
     * @param ciphers 密码套件字符串
     * @return 成功返回 void，失败返回 SslError
     */
    std::expected<void, SslError> set_ciphers(const std::string& ciphers);

    /**
     * @brief 设置密码套件（TLS 1.3）
     *
     * @param ciphersuites TLS 1.3 密码套件字符串
     * @return 成功返回 void，失败返回 SslError
     */
    std::expected<void, SslError> set_ciphersuites(const std::string& ciphersuites);

    /**
     * @brief 设置 ALPN 协议列表
     *
     * @param protocols 协议列表，如 {"h2", "http/1.1"}
     * @return 成功返回 void，失败返回 SslError
     */
    std::expected<void, SslError> set_alpn_protocols(const std::vector<std::string>& protocols);

    /**
     * @brief 设置服务端 ALPN 选择列表
     *
     * @param protocols 按优先级排列的协议列表，如 {"h2", "http/1.1"}
     * @return 成功返回 void，失败返回 SslError
     */
    std::expected<void, SslError> set_alpn_select_protocols(const std::vector<std::string>& protocols);

    /**
     * @brief 设置最小 TLS 版本
     * @param version TLS 版本（如 TLS1_2_VERSION）
     */
    void set_min_protocol_version(int version);

    /**
     * @brief 设置最大 TLS 版本
     * @param version TLS 版本（如 TLS1_3_VERSION）
     */
    void set_max_protocol_version(int version);

    /**
     * @brief 启用会话缓存
     * @param mode 缓存模式
     */
    void set_session_cache_mode(long mode);

    /**
     * @brief 设置会话超时时间
     * @param timeout 超时秒数
     */
    void set_session_timeout(long timeout);

    /**
     * @brief 关闭 SSL 会话缓存
     */
    void disable_session_cache();

    /**
     * @brief 关闭 TLS session ticket
     */
    void disable_session_tickets();

    /**
     * @brief 获取创建时的错误
     */
    const SslError& error() const { return m_error; }

private:
    static int select_alpn_callback(SSL* ssl,
                                  const unsigned char** out,
                                  unsigned char* outlen,
                                  const unsigned char* in,
                                  unsigned int inlen,
                                  void* arg);
    void refresh_callback_context() noexcept;

    SSL_CTX* m_ctx;                                             ///< OpenSSL SSL_CTX
    SslError m_error;                                           ///< 创建时的错误
    std::function<bool(bool, X509_STORE_CTX*)> m_verify_callback;///< 验证回调
    std::vector<std::string> m_alpnSelectProtocols;             ///< 服务端 ALPN 选择优先级
};

} // namespace galay::ssl

#endif // GALAY_SSL_CONTEXT_H
