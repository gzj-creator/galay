/**
 * @file http_client.h
 * @brief HTTP/HTTPS 客户端
 * @author galay-http
 * @version 1.0.0
 *
 * @details 提供 HTTP 与 HTTPS 客户端的模板实现，支持 URL 解析、
 *          TCP/TLS 连接、Session 创建与 Socket 所有权转移（协议升级）。
 */

#ifndef GALAY_HTTP_CLIENT_H
#define GALAY_HTTP_CLIENT_H

#include "galay-http/kernel/http_session.h"
#include "galay-http/common/http_log.h"
#include "galay-kernel/async/tcp_socket.h"
#include "galay-kernel/core/task.h"
#include "galay-http/protoc/http_header.h"
#include <algorithm>
#include <cerrno>
#include <cctype>
#include <charconv>
#include <expected>
#include <functional>
#include <memory>
#include <string>
#include <system_error>
#include <optional>

namespace galay::websocket {
    template<typename SocketType>
    class WsConnImpl;
}

namespace galay::http
{

using namespace galay::async;
using namespace galay::kernel;

/**
 * @brief HTTP URL 解析结果
 */
struct HttpUrl {
    std::string scheme;    ///< 协议（http/https）
    std::string host;      ///< 主机名
    int port;              ///< 端口号
    std::string path;      ///< 路径
    bool is_secure;        ///< 是否为安全连接（HTTPS）

    /**
     * @brief 从 URL 字符串解析各组成部分
     * @param url 完整的 URL 字符串
     * @return 解析成功返回 HttpUrl，失败返回 std::nullopt
     */
    static std::optional<HttpUrl> parse(const std::string& url) {
        const auto scheme_end = url.find("://");
        if (scheme_end == std::string::npos || scheme_end == 0) {
            return std::nullopt;
        }

        HttpUrl result;
        result.scheme = url.substr(0, scheme_end);
        std::transform(result.scheme.begin(), result.scheme.end(), result.scheme.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (result.scheme != "http" && result.scheme != "https") {
            return std::nullopt;
        }
        result.is_secure = (result.scheme == "https");

        const size_t authority_begin = scheme_end + 3;
        const size_t path_begin = url.find('/', authority_begin);
        const std::string authority = path_begin == std::string::npos
            ? url.substr(authority_begin)
            : url.substr(authority_begin, path_begin - authority_begin);
        if (authority.empty()) {
            return std::nullopt;
        }

        const size_t colon = authority.find(':');
        if (colon == std::string::npos) {
            result.host = authority;
            result.port = result.is_secure ? 443 : 80;
        } else {
            result.host = authority.substr(0, colon);
            const std::string port_text = authority.substr(colon + 1);
            if (result.host.empty() || port_text.empty()) {
                return std::nullopt;
            }
            int port = 0;
            const auto* begin = port_text.data();
            const auto* end = begin + port_text.size();
            auto [ptr, ec] = std::from_chars(begin, end, port);
            if (ec != std::errc{} || ptr != end || port <= 0 || port > 65535) {
                return std::nullopt;
            }
            result.port = port;
        }

        if (result.host.empty()) {
            return std::nullopt;
        }

        if (path_begin == std::string::npos) {
            result.path = "/";
        } else {
            result.path = url.substr(path_begin);
        }

        return result;
    }
};

// 前向声明
template<typename SocketType>
class HttpClientImpl;

/**
 * @brief HTTP客户端配置
 * @details
 * - `header_mode` 控制 HeaderPair 的大小写/归一化策略。
 * - 配置会在 `build()` 时复制到客户端对象；后续修改 builder 不影响已构建实例。
 */
struct HttpClientConfig
{
    HeaderPair::Mode header_mode = HeaderPair::Mode::ClientSide;
};

/**
 * @brief HTTP 客户端 builder
 * @details builder 本身不持有 socket 或网络资源，只负责收集构造参数。
 */
class HttpClientBuilder {
public:
    HttpClientBuilder& headerMode(HeaderPair::Mode v) { m_config.header_mode = v; return *this; }
    HttpClientImpl<TcpSocket> build() const;
    HttpClientConfig buildConfig() const                       { return m_config; }
private:
    HttpClientConfig m_config;
};

/**
 * @brief HTTP客户端模板类
 * @details
 * 典型调用顺序：
 * 1. `connect(url)`
 * 2. `getSession()` 或直接访问 `socket()`
 * 3. 通过 `HttpSessionImpl` 发起请求/读取响应
 * 4. `close()`，或让底层 socket 在析构路径上释放
 *
 * 所有权说明：
 * - 客户端独占持有一个 `SocketType`
 * - `getSession()` 返回的 Session 只借用 socket，不转移所有权
 * - `releaseSocket()` 会把 socket 所有权转移给调用方，适合协议升级场景
 *
     * 失败语义：
     * - URL 非法、协议与客户端类型不匹配、socket 初始化失败、网络连接失败都通过返回值传播
     * - 不通过异常向业务层报告运行期错误
 */
template<typename SocketType>
class HttpClientImpl
{
public:
    HttpClientImpl(const HttpClientConfig& config = HttpClientConfig())
        : m_socket(nullptr)
        , m_config(config)
    {
    }

    HttpClientImpl(SocketType&& socket, const HttpClientConfig& config = HttpClientConfig())
        : m_socket(std::make_unique<SocketType>(std::move(socket)))
        , m_config(config)
    {
    }

    ~HttpClientImpl() = default;

    HttpClientImpl(const HttpClientImpl&) = delete;
    HttpClientImpl& operator=(const HttpClientImpl&) = delete;
    HttpClientImpl(HttpClientImpl&&) noexcept = default;
    HttpClientImpl& operator=(HttpClientImpl&&) noexcept = default;

    /**
     * @brief 解析 URL 并发起 TCP 连接
     * @param url 形如 `http://host[:port][/path]` 的目标地址
     * @return 连接任务；成功后可通过 `url()` 读取解析结果
     * @note 对明文 `HttpClient` 而言只接受 `http://`；若传入 `https://` 请改用 `HttpsClient`
     */
    Task<std::expected<void, IOError>> connect(const std::string& url) {
        auto parsed_url = HttpUrl::parse(url);
        if (!parsed_url) {
            co_return std::unexpected(IOError(kParamInvalid, 0));
        }

        m_url = parsed_url.value();

        if constexpr (std::is_same_v<SocketType, TcpSocket>) {
            if (m_url.is_secure) {
                co_return std::unexpected(IOError(kParamInvalid, 0));
            }
        }

        try {
            m_socket = std::make_unique<SocketType>(IPType::IPV4);
        } catch (...) {
            m_socket.reset();
            co_return std::unexpected(IOError(kOpenFailed, errno));
        }

        auto nonblock_result = m_socket->option().handleNonBlock();
        if (!nonblock_result) {
            m_socket.reset();
            co_return std::unexpected(nonblock_result.error());
        }

        Host server_host(IPType::IPV4, m_url.host, m_url.port);
        auto connect_result = co_await m_socket->connect(server_host);
        if (!connect_result) {
            m_socket.reset();
            co_return std::unexpected(connect_result.error());
        }
        co_return std::expected<void, IOError>{};
    }

    /**
     * @brief 创建一个借用当前 socket 的 HTTP session
     * @param ring_buffer_size Session 内部 RingBuffer 大小
     * @param reader_setting Reader 行为配置
     * @param writer_setting Writer 行为配置
     * @return 成功返回 Session 指针；未连接返回 HttpError
     * @note 只要 Session 仍在使用，就必须保证客户端对象和内部 socket 继续存活
     */
    std::expected<std::unique_ptr<HttpSessionImpl<SocketType>>, HttpError>
    getSession(size_t ring_buffer_size = 8192,
               const HttpReaderSetting& reader_setting = HttpReaderSetting(),
               const HttpWriterSetting& writer_setting = HttpWriterSetting()) {
        if (!m_socket) {
            return std::unexpected(HttpError(kTcpConnectError, "Client not connected"));
        }
        return std::make_unique<HttpSessionImpl<SocketType>>(
            *m_socket, ring_buffer_size, reader_setting, writer_setting);
    }

    /**
     * @brief 主动关闭底层 socket
     * @return 底层 socket 的 close awaitable
     * @note 如果已经通过 `releaseSocket()` 转移所有权，则不应再调用该函数
     */
    Task<std::expected<void, IOError>> close() {
        if (!m_socket) {
            co_return std::unexpected(IOError(kNotReady, 0));
        }
        co_return co_await m_socket->close();
    }

    std::expected<std::reference_wrapper<SocketType>, HttpError> socket() {
        if (!m_socket) {
            return std::unexpected(HttpError(kTcpConnectError, "Client not connected"));
        }
        return std::ref(*m_socket);
    }
    const HttpUrl& url() const { return m_url; }

    /**
     * @brief 释放底层 socket 的所有权
     * @return 一个 `unique_ptr<SocketType>`；调用后客户端不再拥有 socket
     * @details 用于 HTTP -> WebSocket 等协议升级，调用方需负责后续关闭与生命周期管理
     */
    std::unique_ptr<SocketType> releaseSocket() { return std::move(m_socket); }

protected:
    std::unique_ptr<SocketType> m_socket;
    HttpClientConfig m_config;
    HttpUrl m_url;
};

// 类型别名 - HTTP (TcpSocket)
using HttpClient = HttpClientImpl<TcpSocket>;
inline HttpClient HttpClientBuilder::build() const { return HttpClient(m_config); }

} // namespace galay::http

#ifdef GALAY_SSL_FEATURE_ENABLED
#include "galay-ssl/async/ssl_socket.h"
#include "galay-ssl/ssl/ssl_context.h"

namespace galay::http {

/**
 * @brief HTTPS 客户端配置
 * @details
 * - `ca_path` 为空时不额外加载 CA 文件
 * - `verify_peer=false` 时不会校验证书链，适合本地自签名测试，不适合生产环境
 * - `header_mode` 与明文 `HttpClientConfig` 的语义保持一致
 */
struct HttpsClientConfig
{
    // SSL 配置
    std::string ca_path;            // CA 证书路径（可选，用于验证服务器）
    bool verify_peer = false;       // 是否验证服务器证书
    int verify_depth = 4;           // 证书链验证深度
    HeaderPair::Mode header_mode = HeaderPair::Mode::ClientSide;
};

class HttpsClient;

/**
 * @brief HTTPS 客户端 builder
 * @details builder 只负责收集 TLS 与 HTTP 头部策略配置，不直接建立网络连接。
 */
class HttpsClientBuilder {
public:
    HttpsClientBuilder& caPath(std::string v)              { m_config.ca_path = std::move(v); return *this; }
    HttpsClientBuilder& verifyPeer(bool v)                 { m_config.verify_peer = v; return *this; }
    HttpsClientBuilder& verifyDepth(int v)                 { m_config.verify_depth = v; return *this; }
    HttpsClientBuilder& headerMode(HeaderPair::Mode v) { m_config.header_mode = v; return *this; }
    HttpsClient build() const;
    HttpsClientConfig buildConfig() const                  { return m_config; }
private:
    HttpsClientConfig m_config;
};

/**
 * @brief HTTPS 客户端类
 * @details
 * 典型调用顺序：
 * 1. `connect(https_url)`
 * 2. `handshake()`
 * 3. `getSession()` 发起 HTTP 请求
 * 4. `close()` 或由上层协议关闭
 *
 * `connect()` 只负责 TCP 连接和 TLS socket 初始化，不会隐式完成 SSL 握手。
 * 若在握手前直接开始读写，会得到未定义的协议行为或底层错误。
 */
class HttpsClient : public HttpClientImpl<galay::ssl::SslSocket>
{
public:
    HttpsClient(const HttpsClientConfig& config = HttpsClientConfig())
        : HttpClientImpl<galay::ssl::SslSocket>(convertConfig(config))
        , m_https_config(config)
        , m_ssl_ctx(galay::ssl::SslMethod::TLS_Client)
    {
        initSslContext();
    }

    ~HttpsClient() = default;

    HttpsClient(const HttpsClient&) = delete;
    HttpsClient& operator=(const HttpsClient&) = delete;
    HttpsClient(HttpsClient&&) noexcept = default;
    HttpsClient& operator=(HttpsClient&&) noexcept = default;

    /**
     * @brief 解析 HTTPS URL、初始化 TLS socket 并发起 TCP 连接
     * @param url 形如 `https://host[:port][/path]` 的目标地址
     * @return 连接任务；URL 非法或 socket 初始化失败时返回错误
     * @note 该函数不会自动执行 TLS 握手；成功连接后仍需显式调用 `handshake()`
     */
    Task<std::expected<void, IOError>> connect(const std::string& url) {
        auto parsed_url = HttpUrl::parse(url);
        if (!parsed_url) {
            co_return std::unexpected(IOError(kParamInvalid, 0));
        }

        m_url = parsed_url.value();

        if (!m_ssl_context_ready) {
            co_return std::unexpected(IOError(kParamInvalid, 0));
        }

        if (!m_url.is_secure) {
            HTTP_LOG_WARN("[connect] [scheme]", "non-secure URL used with HttpsClient");
        }

        try {
            m_socket = std::make_unique<galay::ssl::SslSocket>(&m_ssl_ctx);
        } catch (...) {
            m_socket.reset();
            co_return std::unexpected(IOError(kOpenFailed, errno));
        }
        if (m_socket->handle().fd < 0) {
            m_socket.reset();
            co_return std::unexpected(IOError(kOpenFailed, errno));
        }

        auto nonblock_result = m_socket->option().handleNonBlock();
        if (!nonblock_result) {
            m_socket.reset();
            co_return std::unexpected(nonblock_result.error());
        }

        // 设置 SNI (Server Name Indication)
        auto sni_result = m_socket->setHostname(m_url.host);
        if (!sni_result) {
            HTTP_LOG_WARN("[connect] [sni] [fail]", "host={}", m_url.host);
        }

        Host server_host(IPType::IPV4, m_url.host, m_url.port);
        auto connect_result = co_await m_socket->connect(server_host);
        if (!connect_result) {
            m_socket.reset();
            co_return std::unexpected(connect_result.error());
        }
        co_return std::expected<void, IOError>{};
    }

    /**
     * @brief 执行 SSL 握手（协议完成后再唤醒）
     * @return TLS 握手任务；未连接时返回 SSL 错误
     */
    Task<std::expected<void, galay::ssl::SslError>> handshake() {
        if (!m_socket) {
            co_return std::unexpected(galay::ssl::SslError(galay::ssl::SslErrorCode::kUnknown));
        }
        co_return co_await m_socket->handshake();
    }

    /**
     * @brief 检查握手是否完成
     * @return 已完成握手则返回 true；未连接或握手未完成则返回 false
     */
    bool isHandshakeCompleted() const {
        return m_socket && m_socket->isHandshakeCompleted();
    }

private:
    void initSslContext() {
        if (!m_ssl_ctx.isValid()) {
            m_ssl_context_ready = false;
            return;
        }
        m_ssl_context_ready = true;

        // 加载 CA 证书
        if (!m_https_config.ca_path.empty()) {
            auto result = m_ssl_ctx.loadCACertificate(m_https_config.ca_path);
            if (!result) {
                HTTP_LOG_ERROR("[ssl] [ca] [fail]", "path={}", m_https_config.ca_path);
            }
        }

        // 设置验证模式
        if (m_https_config.verify_peer) {
            m_ssl_ctx.setVerifyMode(galay::ssl::SslVerifyMode::Peer);
            m_ssl_ctx.setVerifyDepth(m_https_config.verify_depth);
        } else {
            m_ssl_ctx.setVerifyMode(galay::ssl::SslVerifyMode::None);
        }
    }

    static HttpClientConfig convertConfig(const HttpsClientConfig& config) {
        HttpClientConfig base_config;
        return base_config;
    }

    HttpsClientConfig m_https_config;
    galay::ssl::SslContext m_ssl_ctx;
    bool m_ssl_context_ready = true;
};

inline HttpsClient HttpsClientBuilder::build() const { return HttpsClient(m_config); }

} // namespace galay::http
#endif

#endif // GALAY_HTTP_CLIENT_H
