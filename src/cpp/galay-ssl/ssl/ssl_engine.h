/**
 * @file ssl_engine.h
 * @brief SSL 引擎
 * @author galay-ssl
 * @version 1.0.0
 *
 * @details 封装单个 SSL 连接的状态和操作，提供非阻塞的握手、读写和关闭接口。
 * 使用 Memory BIO 模式将网络 IO 与 SSL 操作解耦，支持异步事件驱动架构。
 */

#ifndef GALAY_SSL_ENGINE_H
#define GALAY_SSL_ENGINE_H

#include "../common/defn.hpp"
#include "../common/error.h"
#include "ssl_context.h"
#include <expected>
#include <string>

namespace galay::ssl
{

/**
 * @brief SSL 引擎类
 *
 * @details 封装单个 SSL 连接的状态和操作。
 * 每个 SSL 连接需要一个独立的 SslEngine 实例。
 *
 * @note
 * - 不可拷贝，仅支持移动语义
 * - 必须在 TCP 连接建立后使用
 */
class SslEngine
{
public:
    /**
     * @brief 构造 SSL 引擎
     * @param ctx SSL 上下文
     */
    explicit SslEngine(SslContext* ctx);

    /**
     * @brief 析构函数
     */
    ~SslEngine();

    /// @brief 禁用拷贝
    SslEngine(const SslEngine&) = delete;
    SslEngine& operator=(const SslEngine&) = delete;

    /**
     * @brief 移动构造
     */
    SslEngine(SslEngine&& other) noexcept;

    /**
     * @brief 移动赋值
     */
    SslEngine& operator=(SslEngine&& other) noexcept;

    /**
     * @brief 检查引擎是否有效
     */
    bool isValid() const { return m_ssl != nullptr; }

    /**
     * @brief 获取底层 SSL 指针
     */
    SSL* native() const { return m_ssl; }

    /**
     * @brief 设置文件描述符（旧模式，已弃用）
     * @param fd socket 文件描述符
     * @return 成功返回 void，失败返回 SslError
     * @deprecated 使用 initMemoryBIO() 替代
     */
    std::expected<void, SslError> setFd(int fd);

    /**
     * @brief 使用 Memory BIO 初始化（IO 与 SSL 解耦）
     * @return 成功返回 void，失败返回 SslError
     */
    std::expected<void, SslError> initMemoryBIO();

    /**
     * @brief 将从网络 recv 到的密文喂给 SSL（写入 rbio）
     * @param data 密文数据
     * @param length 数据长度
     * @return 实际写入 BIO 的字节数，-1 表示错误
     */
    int feedEncryptedInput(const char* data, size_t length);

    /**
     * @brief 从 SSL 取出待发送的密文（读取 wbio）
     * @param buffer 输出缓冲区
     * @param length 缓冲区大小
     * @return 实际读取的字节数，0 表示无数据，-1 表示错误
     */
    int extractEncryptedOutput(char* buffer, size_t length);

    /**
     * @brief 检查 wbio 中是否有待发送的密文
     * @return 待发送的密文字节数
     */
    size_t pendingEncryptedOutput() const;

    /**
     * @brief 设置 SNI 主机名
     * @param hostname 服务器主机名
     * @return 成功返回 void，失败返回 SslError
     */
    std::expected<void, SslError> setHostname(const std::string& hostname);

    /**
     * @brief 设置为客户端模式
     */
    void setConnectState();

    /**
     * @brief 设置为服务端模式
     */
    void setAcceptState();

    /**
     * @brief 执行握手（非阻塞）
     * @return 握手结果
     */
    SslIOResult doHandshake();

    /**
     * @brief 读取数据（非阻塞）
     * @param buffer 接收缓冲区
     * @param length 缓冲区大小
     * @param bytesRead 输出实际读取的字节数
     * @return IO 结果
     */
    SslIOResult read(char* buffer, size_t length, size_t& bytesRead);

    /**
     * @brief 写入数据（非阻塞）
     * @param buffer 发送数据
     * @param length 数据长度
     * @param bytesWritten 输出实际写入的字节数
     * @return IO 结果
     */
    SslIOResult write(const char* buffer, size_t length, size_t& bytesWritten);

    /**
     * @brief 关闭 SSL 连接（非阻塞）
     * @return IO 结果
     */
    SslIOResult shutdown();

    /**
     * @brief 获取握手状态
     */
    SslHandshakeState handshakeState() const { return m_handshakeState; }

    /**
     * @brief 检查握手是否完成
     */
    bool isHandshakeCompleted() const {
        return m_handshakeState == SslHandshakeState::Completed;
    }

    /**
     * @brief 获取对端证书
     * @return X509 证书指针，需要调用者释放
     */
    X509* getPeerCertificate() const;

    /**
     * @brief 获取证书验证结果
     * @return 验证结果码
     */
    long getVerifyResult() const;

    /**
     * @brief 获取协商的协议版本
     * @return 协议版本字符串
     */
    std::string getProtocolVersion() const;

    /**
     * @brief 获取协商的密码套件
     * @return 密码套件名称
     */
    std::string getCipher() const;

    /**
     * @brief 获取协商的 ALPN 协议
     * @return ALPN 协议名称
     */
    std::string getALPNProtocol() const;

    /**
     * @brief 获取最后一次操作的 SSL 错误
     * @param ret SSL 操作返回值
     * @return SSL 错误码
     */
    int getError(int ret) const;

    /**
     * @brief 获取待发送数据大小
     */
    size_t pending() const;

    /**
     * @brief 设置 Session（用于客户端 Session 复用）
     * @param session SSL_SESSION 指针
     * @return 成功返回 true
     */
    bool setSession(SSL_SESSION* session);

    /**
     * @brief 获取当前 Session（握手完成后调用）
     * @return SSL_SESSION 指针，调用者需要 SSL_SESSION_free
     */
    SSL_SESSION* getSession() const;

    /**
     * @brief 检查是否复用了 Session
     * @return 是否复用
     */
    bool isSessionReused() const;

private:
    SSL* m_ssl;                         ///< OpenSSL SSL 对象
    SslContext* m_ctx;                  ///< SSL 上下文（不拥有）
    BIO* m_rbio = nullptr;             ///< read BIO（网络密文 → SSL）
    BIO* m_wbio = nullptr;             ///< write BIO（SSL → 网络密文）
    SslHandshakeState m_handshakeState; ///< 握手状态
};

} // namespace galay::ssl

#endif // GALAY_SSL_ENGINE_H
