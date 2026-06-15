/**
 * @file ws_client.h
 * @brief WebSocket 客户端，支持 ws:// 和 wss:// 连接与升级
 * @author galay-http
 * @version 1.0.0
 *
 * @details 提供 WsClientImpl 和 WssClient 模板类，支持 WebSocket 连接的
 *          建立、TLS 握手和协议升级。内部使用状态机驱动升级流程。
 */

#ifndef GALAY_WS_CLIENT_H
#define GALAY_WS_CLIENT_H

#include "ws_session.h"
#include "ws_url.h"
#include "galay-ws/kernel/ws_conn.h"
#include "galay-ws/server/ws_upgrade.h"
#include "galay-http/common/iovec_utils.h"
#include "galay-http/protoc/http_header.h"
#include "galay-http/protoc/http_request.h"
#include "galay-http/protoc/http_response.h"
#include "galay-http/builder/http_builder.h"
#include "galay-kernel/async/tcp_socket.h"
#include <galay-utils/cache/bytes.hpp>
#include <galay-utils/cache/ring_buffer.hpp>
#include "galay-kernel/core/awaitable.h"
#include "galay-kernel/core/task.h"
#include <array>
#include <cerrno>
#include <expected>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include <vector>

#ifdef GALAY_SSL_FEATURE_ENABLED
#include "galay-ssl/async/ssl_await.h"
#include "galay-ssl/async/ssl_socket.h"
#include "galay-ssl/ssl/ssl_context.h"
#endif

namespace galay::websocket
{

using namespace galay::async;
using namespace galay::kernel;
using ::galay::utils::Bytes;
using ::galay::utils::RingBuffer;
using namespace galay::http;

template<typename SocketType>
class WsClientImpl;

template<typename SocketType>
class WsUpgraderImpl;

/**
 * @brief WebSocket 客户端配置
 */
struct WsClientConfig
{
    HeaderPair::Mode header_mode = HeaderPair::Mode::ClientSide; ///< HTTP 头部归一化策略
};

/**
 * @brief WebSocket 客户端 builder
 * @details builder 只收集升级请求所需的头部策略，不直接持有网络资源。
 */
class WsClientBuilder {
public:
    WsClientBuilder& headerMode(HeaderPair::Mode v) { m_config.header_mode = v; return *this; }
    WsClientImpl<TcpSocket> build() const;
    WsClientConfig buildConfig() const { return m_config; }

private:
    WsClientConfig m_config;
};

namespace detail {

template<typename SocketType>
struct WsClientUpgradeState {
    using ResultType = std::expected<bool, WsError>;

    WsClientUpgradeState(SocketType* socket,
                         RingBuffer* ring_buffer,
                         WsUrl url,
                         std::unique_ptr<WsConnImpl<SocketType>>* ws_conn_ptr)
        : m_socket(socket)
        , m_ring_buffer(ring_buffer)
        , m_url(std::move(url))
        , m_ws_conn_ptr(ws_conn_ptr)
    {
        initialize();
    }

    bool isFinished() const {
        return m_result.has_value() || m_error.has_value();
    }

    ResultType takeResult() {
        if (m_error.has_value()) {
            return std::unexpected(std::move(*m_error));
        }
        return m_result.value_or(ResultType(true));
    }

    bool hasPendingSend() const {
        return !isFinished() && m_send_offset < m_send_buffer.size();
    }

    const char* sendData() const {
        return m_send_buffer.data() + m_send_offset;
    }

    size_t remainingSendBytes() const {
        return m_send_buffer.size() - m_send_offset;
    }

    void prepareSendWindow() {
        if (!hasPendingSend()) {
            m_send_iovecs[0] = {.iov_base = nullptr, .iov_len = 0};
            return;
        }

        m_send_iovecs[0] = {
            .iov_base = const_cast<char*>(sendData()),
            .iov_len = remainingSendBytes()
        };
    }

    const struct iovec* sendIovecsData() const {
        return m_send_iovecs.data();
    }

    size_t sendIovecsCount() const {
        return hasPendingSend() ? 1 : 0;
    }

    void onBytesSent(size_t sent_bytes) {
        if (sent_bytes == 0) {
            return;
        }
        if (sent_bytes > remainingSendBytes()) {
            setProtocolError("Send progress overflow");
            return;
        }
        m_send_offset += sent_bytes;
    }

    bool prepareRecvWindow() {
        if (!ensureResources()) {
            return false;
        }
        m_write_iovecs = borrowWriteIovecs(*m_ring_buffer);
        return !m_write_iovecs.empty();
    }

    bool prepareRecvWindow(char*& buffer, size_t& length) {
        if (!prepareRecvWindow()) {
            buffer = nullptr;
            length = 0;
            return false;
        }

        if (!IoVecWindow::bindFirstNonEmpty(m_write_iovecs, buffer, length)) {
            buffer = nullptr;
            length = 0;
            return false;
        }

        return length > 0;
    }

    const struct iovec* recvIovecsData() const {
        return m_write_iovecs.data();
    }

    size_t recvIovecsCount() const {
        return m_write_iovecs.size();
    }

    void onBytesReceived(size_t recv_bytes) {
        if (ensureResources()) {
            m_ring_buffer->produce(recv_bytes);
        }
    }

    bool tryParseUpgradeResponse() {
        if (!ensureResources()) {
            return true;
        }

        auto read_iovecs = borrowReadIovecs(*m_ring_buffer);
        if (read_iovecs.empty()) {
            return false;
        }

        std::vector<iovec> parse_iovecs;
        if (IoVecWindow::buildWindow(read_iovecs, parse_iovecs) == 0) {
            return false;
        }

        auto [error_code, consumed] = m_upgrade_response.fromIOVec(parse_iovecs);
        if (consumed > 0) {
            m_ring_buffer->consume(consumed);
        }

        if (error_code == HttpErrorCode::kIncomplete ||
            error_code == HttpErrorCode::kHeaderInComplete) {
            return false;
        }

        if (error_code != HttpErrorCode::kNoError) {
            setProtocolError("Failed to parse upgrade response");
            return true;
        }

        if (!m_upgrade_response.isComplete()) {
            return false;
        }

        if (!validateUpgradeResponse()) {
            return true;
        }

        *m_ws_conn_ptr = std::make_unique<WsConnImpl<SocketType>>(
            std::move(*m_socket),
            std::move(*m_ring_buffer),
            false);
        m_result = true;
        return true;
    }

    void setSendError(const galay::kernel::IOError& io_error) {
        m_error = WsError(kWsSendError, io_error.message());
    }

    void setRecvError(const galay::kernel::IOError& io_error) {
        if (galay::kernel::IOError::contains(io_error.code(), galay::kernel::kDisconnectError)) {
            m_error = WsError(kWsConnectionClosed, io_error.message());
            return;
        }
        m_error = WsError(kWsConnectionError, io_error.message());
    }

#ifdef GALAY_SSL_FEATURE_ENABLED
    void setSslSendError(const galay::ssl::SslError& error) {
        if (error.code() == galay::ssl::SslErrorCode::kPeerClosed) {
            m_error = WsError(kWsConnectionClosed, "Connection closed by peer");
            return;
        }
        m_error = WsError(kWsSendError, error.message());
    }

    void setSslRecvError(const galay::ssl::SslError& error) {
        if (error.code() == galay::ssl::SslErrorCode::kPeerClosed) {
            m_error = WsError(kWsConnectionClosed, "Connection closed by peer");
            return;
        }
        m_error = WsError(kWsConnectionError, error.message());
    }
#endif

    void setProtocolError(std::string message) {
        m_error = WsError(kWsProtocolError, std::move(message));
    }

private:
    void initialize() {
        if (!ensureResources()) {
            return;
        }

        if (*m_ws_conn_ptr != nullptr) {
            m_result = true;
            return;
        }

        m_ws_key = generateWebSocketKey();
        auto request = Http1_1RequestBuilder::get(m_url.path)
            .host(m_url.host + ":" + std::to_string(m_url.port))
            .header("Connection", "Upgrade")
            .header("Upgrade", "websocket")
            .header("Sec-WebSocket-Version", "13")
            .header("Sec-WebSocket-Key", m_ws_key)
            .build();
        m_send_buffer = request.toString();
    }

    bool validateUpgradeResponse() {
        if (m_upgrade_response.header().code() != HttpStatusCode::SwitchingProtocol_101) {
            return setUpgradeFailed(
                "Upgrade failed with status " +
                std::to_string(static_cast<int>(m_upgrade_response.header().code())));
        }

        if (!m_upgrade_response.header().headerPairs().hasKey("Sec-WebSocket-Accept")) {
            return setUpgradeFailed("Missing Sec-WebSocket-Accept header");
        }

        const std::string accept_key =
            m_upgrade_response.header().headerPairs().getValue("Sec-WebSocket-Accept");
        if (accept_key != WsUpgrade::generateAcceptKey(m_ws_key)) {
            return setUpgradeFailed("Invalid Sec-WebSocket-Accept value");
        }

        return true;
    }

    bool setUpgradeFailed(std::string message) {
        m_error = WsError(kWsUpgradeFailed, std::move(message));
        return false;
    }

    bool ensureResources() {
        if (m_socket != nullptr && m_ring_buffer != nullptr && m_ws_conn_ptr != nullptr) {
            return true;
        }

        if (!m_error.has_value()) {
            m_error = WsError(kWsConnectionError, "WsClient not connected. Call connect() first.");
        }
        return false;
    }

    const char* logScheme() const {
        if constexpr (std::is_same_v<SocketType, TcpSocket>) {
            return "ws";
        } else {
            return "wss";
        }
    }

    SocketType* m_socket = nullptr;
    RingBuffer* m_ring_buffer = nullptr;
    WsUrl m_url;
    std::unique_ptr<WsConnImpl<SocketType>>* m_ws_conn_ptr = nullptr;
    std::string m_ws_key;
    std::string m_send_buffer;
    size_t m_send_offset = 0;
    HttpResponse m_upgrade_response;
    std::array<struct iovec, 1> m_send_iovecs{};
    BorrowedIovecs<2> m_write_iovecs;
    std::optional<ResultType> m_result;
    std::optional<WsError> m_error;
};

template<typename StateT>
struct WsClientTcpUpgradeMachine {
    using result_type = typename StateT::ResultType;

    explicit WsClientTcpUpgradeMachine(std::shared_ptr<StateT> state)
        : m_state(std::move(state)) {}

    MachineAction<result_type> advance() {
        if (m_state->isFinished()) {
            return MachineAction<result_type>::complete(m_state->takeResult());
        }

        if (m_state->hasPendingSend()) {
            m_state->prepareSendWindow();
            return MachineAction<result_type>::waitWritev(
                m_state->sendIovecsData(),
                m_state->sendIovecsCount());
        }

        if (m_state->tryParseUpgradeResponse()) {
            return MachineAction<result_type>::complete(m_state->takeResult());
        }

        if (!m_state->prepareRecvWindow()) {
            if (!m_state->isFinished()) {
                m_state->setProtocolError("Upgrade response too large");
            }
            return MachineAction<result_type>::complete(m_state->takeResult());
        }

        return MachineAction<result_type>::waitReadv(
            m_state->recvIovecsData(),
            m_state->recvIovecsCount());
    }

    void onRead(std::expected<size_t, IOError> result) {
        if (!result) {
            m_state->setRecvError(result.error());
            return;
        }

        if (result.value() == 0) {
            m_state->setProtocolError("Connection closed");
            return;
        }

        m_state->onBytesReceived(result.value());
    }

    void onWrite(std::expected<size_t, IOError> result) {
        if (!result) {
            m_state->setSendError(result.error());
            return;
        }

        m_state->onBytesSent(result.value());
    }

    std::shared_ptr<StateT> m_state;
};

#ifdef GALAY_SSL_FEATURE_ENABLED
template<typename StateT>
struct WsClientSslUpgradeMachine {
    using result_type = typename StateT::ResultType;

    explicit WsClientSslUpgradeMachine(std::shared_ptr<StateT> state)
        : m_state(std::move(state)) {}

    galay::ssl::SslMachineAction<result_type> advance() {
        if (m_state->isFinished()) {
            return galay::ssl::SslMachineAction<result_type>::complete(m_state->takeResult());
        }

        if (m_state->hasPendingSend()) {
            return galay::ssl::SslMachineAction<result_type>::send(
                m_state->sendData(),
                m_state->remainingSendBytes());
        }

        if (m_state->tryParseUpgradeResponse()) {
            return galay::ssl::SslMachineAction<result_type>::complete(m_state->takeResult());
        }

        char* recv_buffer = nullptr;
        size_t recv_length = 0;
        if (!m_state->prepareRecvWindow(recv_buffer, recv_length)) {
            if (!m_state->isFinished()) {
                m_state->setProtocolError("Upgrade response too large");
            }
            return galay::ssl::SslMachineAction<result_type>::complete(m_state->takeResult());
        }

        return galay::ssl::SslMachineAction<result_type>::recv(recv_buffer, recv_length);
    }

    void onHandshake(std::expected<void, galay::ssl::SslError>) {}

    void onRecv(std::expected<Bytes, galay::ssl::SslError> result) {
        if (!result) {
            m_state->setSslRecvError(result.error());
            return;
        }

        const size_t recv_bytes = result.value().size();
        if (recv_bytes == 0) {
            m_state->setProtocolError("Connection closed");
            return;
        }

        m_state->onBytesReceived(recv_bytes);
    }

    void onSend(std::expected<size_t, galay::ssl::SslError> result) {
        if (!result) {
            m_state->setSslSendError(result.error());
            return;
        }

        m_state->onBytesSent(result.value());
    }

    void onShutdown(std::expected<void, galay::ssl::SslError>) {}

    std::shared_ptr<StateT> m_state;
};
#endif

template<typename SocketType>
auto buildWsClientUpgradeOperation(SocketType* socket,
                                   RingBuffer* ring_buffer,
                                   const WsUrl& url,
                                   std::unique_ptr<WsConnImpl<SocketType>>* ws_conn_ptr) {
    using StateT = WsClientUpgradeState<SocketType>;
    using ResultType = typename StateT::ResultType;

    auto state = std::make_shared<StateT>(socket, ring_buffer, url, ws_conn_ptr);

    if constexpr (std::is_same_v<SocketType, TcpSocket>) {
        IOController* controller = socket != nullptr ? socket->controller() : nullptr;
        if (ws_conn_ptr != nullptr && *ws_conn_ptr != nullptr) {
            controller = (*ws_conn_ptr)->socket().controller();
        }

        return AwaitableBuilder<ResultType>::fromStateMachine(
                   controller,
                   WsClientTcpUpgradeMachine<StateT>(std::move(state)))
            .build();
    } else {
#ifdef GALAY_SSL_FEATURE_ENABLED
        auto* active_socket = socket;
        if (ws_conn_ptr != nullptr && *ws_conn_ptr != nullptr) {
            active_socket = &(*ws_conn_ptr)->socket();
        }

        return galay::ssl::SslAwaitableBuilder<ResultType>::fromStateMachine(
                   active_socket->controller(),
                   active_socket,
                   WsClientSslUpgradeMachine<StateT>(std::move(state)))
            .build();
#else
        static_assert(!sizeof(SocketType), "SSL support is disabled");
#endif
    }
}

} // namespace detail

template<typename SocketType>
class WsUpgraderImpl
{
public:
    WsUpgraderImpl(SocketType* socket,
                   RingBuffer* ring_buffer,
                   const WsUrl& url,
                   const WsReaderSetting& reader_setting,
                   const WsWriterSetting& writer_setting,
                   std::unique_ptr<WsConnImpl<SocketType>>* ws_conn_ptr)
        : m_socket(socket)
        , m_ring_buffer(ring_buffer)
        , m_url(url)
        , m_reader_setting(reader_setting)
        , m_writer_setting(writer_setting)
        , m_ws_conn_ptr(ws_conn_ptr)
    {
    }

    Task<std::expected<bool, WsError>> operator()() {
        if (m_socket == nullptr || m_ring_buffer == nullptr || m_ws_conn_ptr == nullptr) {
            co_return std::unexpected(WsError(
                kWsConnectionError,
                "WsClient not connected. Call connect() first."));
        }

        try {
            auto operation = detail::buildWsClientUpgradeOperation(
                m_socket,
                m_ring_buffer,
                m_url,
                m_ws_conn_ptr);
            co_return co_await operation;
        } catch (const std::exception& ex) {
            co_return std::unexpected(WsError(kWsConnectionError, ex.what()));
        } catch (...) {
            co_return std::unexpected(WsError(kWsConnectionError, "WebSocket upgrade exception"));
        }
    }

private:
    SocketType* m_socket;
    RingBuffer* m_ring_buffer;
    const WsUrl& m_url;
    const WsReaderSetting& m_reader_setting;
    const WsWriterSetting& m_writer_setting;
    std::unique_ptr<WsConnImpl<SocketType>>* m_ws_conn_ptr;
};

template<typename SocketType>
class WsClientImpl
{
public:
    explicit WsClientImpl(const WsClientConfig& config = WsClientConfig())
        : m_socket(nullptr)
        , m_config(config)
    {
    }

    ~WsClientImpl() = default;

    WsClientImpl(const WsClientImpl&) = delete;
    WsClientImpl& operator=(const WsClientImpl&) = delete;
    WsClientImpl(WsClientImpl&&) noexcept = default;
    WsClientImpl& operator=(WsClientImpl&&) noexcept = default;

    /**
     * @brief 发起到底层 WebSocket 目标的 TCP 连接
     * @param url 形如 `ws://host[:port][/path]` 的目标地址
     * @return 连接任务；失败通过 expected 错误传播
     * @note 对明文 `WsClient` 而言，若 URL 为 `wss://`，必须改用 `WssClient`
     */
    Task<std::expected<void, IOError>> connect(const std::string& url) {
        auto parsed_url = WsUrl::parse(url);
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
     * @brief 创建一个绑定当前 socket 与 URL 的 WebSocket session
     * @param writer_setting Writer 行为配置
     * @param ring_buffer_size Session RingBuffer 大小
     * @param reader_setting Reader 行为配置
     * @return 成功返回 Session 指针；未连接返回 WsError
     * @details 客户端典型顺序是：`connect()` -> `getSession()` -> `session.upgrade()`
     */
    std::expected<std::unique_ptr<WsSessionImpl<SocketType>>, WsError>
    getSession(const WsWriterSetting& writer_setting,
               size_t ring_buffer_size = 8192,
               const WsReaderSetting& reader_setting = WsReaderSetting()) {
        if (!m_socket) {
            return std::unexpected(WsError(kWsConnectionError, "WsClient not connected. Call connect() first."));
        }
        return std::make_unique<WsSessionImpl<SocketType>>(
            *m_socket, m_url, writer_setting, ring_buffer_size, reader_setting);
    }

    /**
     * @brief 关闭底层 socket
     * @return 关闭任务；未连接返回 IOError
     */
    Task<std::expected<void, IOError>> close() {
        if (!m_socket) {
            co_return std::unexpected(IOError(kNotReady, 0));
        }
        co_return co_await m_socket->close();
    }

    SocketType* getSocket() {
        return m_socket.get();
    }

    /**
     * @brief 对支持 TLS 的 socket 执行握手
     * @return 明文客户端直接成功；未连接返回 IOError
     * @note 明文 `WsClient` 一般不需要显式调用；`WssClient` 则应在升级前先完成 TLS 握手
     */
    Task<std::expected<void, IOError>> handshake()
    requires std::is_same_v<SocketType, TcpSocket>
    {
        if (!m_socket) {
            co_return std::unexpected(IOError(kNotReady, 0));
        }
        co_return std::expected<void, IOError>{};
    }

    /**
     * @brief 检查底层握手是否已经完成
     * @return 未连接返回 false；明文 socket 视为已完成；TLS socket 返回真实握手状态
     */
    bool isHandshakeCompleted() const {
        if (!m_socket) {
            return false;
        }
        if constexpr (requires { m_socket->isHandshakeCompleted(); }) {
            return m_socket->isHandshakeCompleted();
        }
        return true;
    }

    const WsUrl& url() const { return m_url; }

protected:
    std::unique_ptr<SocketType> m_socket;
    WsClientConfig m_config;
    WsUrl m_url;
};

using WsUpgrader = WsUpgraderImpl<TcpSocket>;
using WsClient = WsClientImpl<TcpSocket>;
inline WsClient WsClientBuilder::build() const { return WsClient(m_config); }

#ifdef GALAY_SSL_FEATURE_ENABLED
/**
 * @brief WSS 客户端配置
 */
struct WssClientConfig
{
    std::string ca_path;            ///< CA 证书路径
    bool verify_peer = false;       ///< 是否校验服务端证书
    int verify_depth = 4;           ///< 证书链校验深度
    HeaderPair::Mode header_mode = HeaderPair::Mode::ClientSide; ///< HTTP 头部归一化策略
};

class WssClient;

/**
 * @brief WSS 客户端 builder
 * @details 用于配置 TLS 验证策略和升级请求头部策略。
 */
class WssClientBuilder {
public:
    WssClientBuilder& caPath(std::string v) { m_config.ca_path = std::move(v); return *this; }
    WssClientBuilder& verifyPeer(bool v) { m_config.verify_peer = v; return *this; }
    WssClientBuilder& verifyDepth(int v) { m_config.verify_depth = v; return *this; }
    WssClientBuilder& headerMode(HeaderPair::Mode v) { m_config.header_mode = v; return *this; }
    WssClient build() const;
    WssClientConfig buildConfig() const { return m_config; }

private:
    WssClientConfig m_config;
};

class WssClient : public WsClientImpl<galay::ssl::SslSocket>
{
public:
    WssClient(const WssClientConfig& config = WssClientConfig())
        : WsClientImpl<galay::ssl::SslSocket>()
        , m_wss_config(config)
        , m_ssl_ctx(galay::ssl::SslMethod::TLS_Client)
    {
        initSslContext();
    }

    ~WssClient() = default;

    WssClient(const WssClient&) = delete;
    WssClient& operator=(const WssClient&) = delete;
    WssClient(WssClient&&) noexcept = default;
    WssClient& operator=(WssClient&&) noexcept = default;

    /**
     * @brief 解析 WSS URL、初始化 TLS socket 并发起 TCP 连接
     * @param url 形如 `wss://host[:port][/path]` 的目标地址
     * @return 连接任务；失败通过 expected 错误传播
     * @note 该函数不会自动完成 TLS 握手；成功连接后仍应先 `handshake()` 再执行 `session.upgrade()`
     */
    Task<std::expected<void, IOError>> connect(const std::string& url) {
        auto parsed_url = WsUrl::parse(url);
        if (!parsed_url) {
            co_return std::unexpected(IOError(kParamInvalid, 0));
        }

        m_url = parsed_url.value();

        if (!m_ssl_context_ready) {
            co_return std::unexpected(IOError(kParamInvalid, 0));
        }

        if (!m_url.is_secure) {
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

        auto sni_result = m_socket->setHostname(m_url.host);
        if (!sni_result) {
        }

        Host server_host(IPType::IPV4, m_url.host, m_url.port);
        auto connect_result = co_await m_socket->connect(server_host);
        if (!connect_result) {
            m_socket.reset();
            co_return std::unexpected(connect_result.error());
        }
        co_return std::expected<void, IOError>{};
    }

    Task<std::expected<void, galay::ssl::SslError>> handshake() {
        if (!m_socket) {
            co_return std::unexpected(galay::ssl::SslError(galay::ssl::SslErrorCode::kUnknown));
        }
        co_return co_await m_socket->handshake();
    }

    bool isHandshakeCompleted() const {
        if (!m_socket) {
            return false;
        }
        return m_socket->isHandshakeCompleted();
    }

private:
    void initSslContext() {
        if (!m_ssl_ctx.isValid()) {
            m_ssl_context_ready = false;
            return;
        }
        m_ssl_context_ready = true;

        if (!m_wss_config.ca_path.empty()) {
            auto result = m_ssl_ctx.loadCACertificate(m_wss_config.ca_path);
            if (!result) {
            }
        }

        if (m_wss_config.verify_peer) {
            m_ssl_ctx.setVerifyMode(galay::ssl::SslVerifyMode::Peer);
            m_ssl_ctx.setVerifyDepth(m_wss_config.verify_depth);
        } else {
            m_ssl_ctx.setVerifyMode(galay::ssl::SslVerifyMode::None);
        }
    }

    WssClientConfig m_wss_config;
    galay::ssl::SslContext m_ssl_ctx;
    bool m_ssl_context_ready = true;
};

using WssUpgrader = WsUpgraderImpl<galay::ssl::SslSocket>;
inline WssClient WssClientBuilder::build() const { return WssClient(m_config); }
#endif

} // namespace galay::websocket

#endif // GALAY_WS_CLIENT_H
