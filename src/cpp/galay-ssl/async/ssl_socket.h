/**
 * @file ssl_socket.h
 * @brief 异步 SSL Socket
 * @author galay-ssl
 * @version 1.0.0
 *
 * @details 封装 SSL/TLS 加密的异步 TCP Socket，提供协程友好的异步 IO 接口，
 * 支持连接、握手、收发和关闭等操作，使用 Memory BIO 模式解耦网络 IO 与 SSL。
 */

#ifndef GALAY_SSL_SOCKET_H
#define GALAY_SSL_SOCKET_H

#include "../common/error.h"
#include "../ssl/ssl_context.h"
#include "../ssl/ssl_engine.h"
#include "awaitable.h"
#include "../../galay-kernel/common/defn.hpp"
#include "../../galay-kernel/common/host.hpp"
#include "../../galay-kernel/common/handle_option.h"
#include "../../galay-kernel/core/io_scheduler.hpp"
#include "../../galay-kernel/core/awaitable.h"
#include <expected>
#include <vector>

namespace galay::ssl
{

using namespace galay::kernel;

/**
 * @brief 异步 SSL Socket 类
 *
 * @details 封装 SSL/TLS 加密的 TCP Socket，提供协程友好的异步 IO 接口。
 * 内部包含：
 * - GHandle: 底层 socket 句柄
 * - IOScheduler*: IO 调度器指针
 * - IOController: IO 事件控制器
 * - SslEngine: SSL 引擎
 *
 * @example
 * @code
 * // SSL 服务端
 * Task<void> sslServer(SslContext* ctx) {
 *     SslSocket listener(ctx);
 *     listener.option().handleReuseAddr();
 *     listener.option().handleNonBlock();
 *     listener.bind(Host(IPType::IPV4, "0.0.0.0", 8443));
 *     listener.listen(1024);
 *
 *     while (true) {
 *         Host clientHost;
 *         auto result = co_await listener.accept(&clientHost);
 *         if (result) {
 *             // 处理新连接
 *         }
 *     }
 * }
 *
 * // SSL 客户端
 * Task<void> sslClient(SslContext* ctx) {
 *     SslSocket socket(ctx);
 *     socket.option().handleNonBlock();
 *
 *     co_await socket.connect(Host(IPType::IPV4, "127.0.0.1", 8443));
 *     co_await socket.handshake();
 *
 *     co_await socket.send("Hello", 5);
 *
 *     char buffer[1024];
 *     auto result = co_await socket.recv(buffer, sizeof(buffer));
 *
 *     co_await socket.shutdown();
 *     co_await socket.close();
 * }
 * @endcode
 *
 * @note
 * - 不可拷贝，仅支持移动语义
 * - 析构时不会自动关闭 socket，需显式调用 close()
 * - 所有异步操作需要在协程中使用 co_await
 * - 同一个 SslSocket 当前应由单一上层 I/O owner 串行驱动；不要在同一连接上并发挂多个
 *   SSL sequence/state-machine awaitable（例如一个 reader 协程加一个 writer 协程各自独立 co_await
 *   SSL 状态机），否则会与底层 controller/SSL 驱动的单 owner 语义冲突
 */
class SslSocket
{
public:
    /**
     * @brief 构造函数，创建 SSL Socket
     * @param ctx SSL 上下文指针，不能为 nullptr
     * @param type IP 协议类型，默认 IPv4
     * @note 构造时自动创建底层 socket
     */
    SslSocket(SslContext* ctx, IPType type = IPType::IPV4);

    /**
     * @brief 从已有句柄构造 SSL Socket
     * @param ctx SSL 上下文指针，不能为 nullptr
     * @param handle 已有的 socket 句柄（如 accept 返回的句柄）
     * @note 用于包装已存在的 socket，如服务端 accept 得到的客户端连接
     */
    SslSocket(SslContext* ctx, GHandle handle);

    /**
     * @brief 析构函数
     * @note 不会自动关闭 socket，需显式调用 close()
     */
    ~SslSocket();

    /// @brief 禁用拷贝
    SslSocket(const SslSocket&) = delete;
    SslSocket& operator=(const SslSocket&) = delete;

    /**
     * @brief 移动构造函数
     */
    SslSocket(SslSocket&& other) noexcept;

    /**
     * @brief 移动赋值运算符
     */
    SslSocket& operator=(SslSocket&& other) noexcept;

    /**
     * @brief 获取底层 socket 句柄
     */
    GHandle handle() const { return m_controller.m_handle; }

    /**
     * @brief 获取 IO 控制器指针
     */
    IOController* controller() { return &m_controller; }

    /**
     * @brief 获取 SSL 引擎指针
     */
    SslEngine* engine() { return &m_engine; }

    /**
     * @brief 检查 socket 是否有效
     */
    bool isValid() const { return m_controller.m_handle.fd >= 0 && m_engine.isValid(); }

    /**
     * @brief 检查 SSL 握手是否完成
     */
    bool isHandshakeCompleted() const { return m_engine.isHandshakeCompleted(); }

    /**
     * @brief 绑定本地地址
     *
     * @param host 要绑定的地址
     * @return 成功返回 void，失败返回 IOError
     */
    std::expected<void, IOError> bind(const Host& host);

    /**
     * @brief 开始监听连接
     *
     * @param backlog 等待连接队列的最大长度
     * @return 成功返回 void，失败返回 IOError
     */
    std::expected<void, IOError> listen(int backlog = 128);

    /**
     * @brief 获取句柄选项配置器
     */
    HandleOption option() { return HandleOption(m_controller.m_handle); }

    /**
     * @brief 设置 SNI 主机名（客户端使用）
     *
     * @param hostname 服务器主机名
     * @return 成功返回 void，失败返回 SslError
     */
    std::expected<void, SslError> setHostname(const std::string& hostname);

    /**
     * @brief 异步接受新连接
     *
     * @param clientHost 输出参数，接收客户端地址信息
     * @return AcceptAwaitable 可等待对象
     *
     * @note 返回的是原始 TCP 连接，需要创建新的 SslSocket 并执行握手
     */
    AcceptAwaitable accept(Host* clientHost);

    /**
     * @brief 异步连接到服务器
     *
     * @param host 目标服务器地址
     * @return ConnectAwaitable 可等待对象
     *
     * @note 连接成功后需要调用 handshake() 执行 SSL 握手
     */
    ConnectAwaitable connect(const Host& host);

    /**
     * @brief 异步执行 SSL 握手
     *
     * @return SslHandshakeAwaitable 可等待对象
     *
     * @note
     * - 客户端：在 connect() 成功后调用
     * - 服务端：在 accept() 后创建新 SslSocket 并调用
     */
    SslHandshakeAwaitable handshake();

    /**
     * @brief 异步接收数据
     *
     * @param buffer 接收缓冲区指针
     * @param length 缓冲区大小
     * @return SslRecvAwaitable 可等待对象
     *
     * @note 必须在握手完成后调用
     */
    SslRecvAwaitable recv(char* buffer, size_t length);

    /**
     * @brief 异步发送数据
     *
     * @param buffer 发送数据指针
     * @param length 数据长度
     * @return SslSendAwaitable 可等待对象
     *
     * @note 必须在握手完成后调用
     */
    SslSendAwaitable send(const char* buffer, size_t length);

    /**
     * @brief 异步关闭 SSL 连接
     *
     * @return SslShutdownAwaitable 可等待对象
     *
     * @note 执行 SSL 关闭握手，之后需要调用 close() 关闭底层 socket
     */
    SslShutdownAwaitable shutdown();

    /**
     * @brief 异步关闭底层 socket
     *
     * @return CloseAwaitable 可等待对象
     */
    CloseAwaitable close();

    /**
     * @brief 获取对端证书
     * @return X509 证书指针，需要调用者释放
     */
    X509* getPeerCertificate() const { return m_engine.getPeerCertificate(); }

    /**
     * @brief 获取证书验证结果
     */
    long getVerifyResult() const { return m_engine.getVerifyResult(); }

    /**
     * @brief 获取协商的协议版本
     */
    std::string getProtocolVersion() const { return m_engine.getProtocolVersion(); }

    /**
     * @brief 获取协商的密码套件
     */
    std::string getCipher() const { return m_engine.getCipher(); }

    /**
     * @brief 获取协商的 ALPN 协议
     */
    std::string getALPNProtocol() const { return m_engine.getALPNProtocol(); }

    /**
     * @brief 设置 Session（用于客户端 Session 复用）
     * @param session SSL_SESSION 指针
     * @return 成功返回 true
     */
    bool setSession(SSL_SESSION* session) { return m_engine.setSession(session); }

    /**
     * @brief 获取当前 Session（握手完成后调用）
     * @return SSL_SESSION 指针，调用者需要 SSL_SESSION_free
     */
    SSL_SESSION* getSession() const { return m_engine.getSession(); }

    /**
     * @brief 检查是否复用了 Session
     * @return 是否复用
     */
    bool isSessionReused() const { return m_engine.isSessionReused(); }

private:
    /**
     * @brief 初始化 SSL 引擎
     * @return 成功返回 true
     */
    bool initEngine();

private:
    friend class SslOperationDriver;

    IOController m_controller;  ///< IO 事件控制器
    SslContext* m_ctx;          ///< SSL 上下文（不拥有）
    SslEngine m_engine;         ///< SSL 引擎
    bool m_isServer;            ///< 是否为服务端模式
    bool m_engineInitialized;   ///< SSL 引擎是否已初始化
    std::vector<char> m_handshakeBuffer;   ///< 握手阶段的密文缓冲区
    std::vector<char> m_shutdownBuffer;    ///< 关闭阶段的密文缓冲区
    std::vector<char> m_recvCipherBuffer;  ///< 接收密文缓冲区
    std::vector<char> m_sendCipherBuffer;  ///< 发送密文缓冲区
};

} // namespace galay::ssl

#endif // GALAY_SSL_SOCKET_H
