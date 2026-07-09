/**
 * @file h2_client.h
 * @brief HTTP/2 客户端，支持多路复用流和 HPACK 头部压缩
 * @author galay-http
 * @version 1.0.0
 *
 * @details 提供 H2Client 模板类，支持 HTTP/2 协议的客户端功能，
 *          包括 SETTINGS 握手、流管理和请求发送。
 */

#ifndef GALAY_H2_CLIENT_H
#define GALAY_H2_CLIENT_H

#include "../kernel/http2_conn.h"
#include "../kernel/http2_stream.h"
#include "../../galay-http/common/http_log.h"
#include "../../galay-http/common/iovec_utils.h"
#include "../protoc/http2_base.h"
#include "../protoc/http2_frame.h"
#include "../../galay-kernel/core/awaitable.h"
#include "../../galay-kernel/core/task.h"
#include "../../galay-kernel/core/timeout.hpp"
#ifdef GALAY_SSL_FEATURE_ENABLED
#include "../../galay-ssl/async/ssl_await.h"
#include "../../galay-ssl/async/ssl_socket.h"
#include "../../galay-ssl/ssl/ssl_context.h"
#endif
#include <array>
#include <charconv>
#include <cstring>
#include <memory>
#include <optional>
#include <vector>

namespace galay::http2
{

#ifdef GALAY_SSL_FEATURE_ENABLED
using namespace galay::kernel;
using ::galay::utils::Bytes;
using ::galay::utils::RingBuffer;
using ::galay::utils::RingBufferBackendStrategy;

struct H2ClientConfig
{
    bool tcp_no_delay = true;
    uint32_t max_concurrent_streams = 100;
    uint32_t initial_window_size = 65535;
    uint32_t max_frame_size = 16384;
    uint32_t max_header_list_size = 8192;
    bool verify_peer = false;
    std::string ca_path;
};

template<RingBufferBackendStrategy Strategy = RingBufferBackendStrategy::Mmap>
class H2Client;

namespace detail
{
template<RingBufferBackendStrategy Strategy>
inline bool parseFrameFromRingBuffer(RingBuffer<Strategy>& ring_buffer,
                                     uint32_t max_frame_size,
                                     std::optional<Http2ErrorCode>& error,
                                     std::vector<uint8_t>& scratch,
                                     Http2Frame::uptr& frame);

template<RingBufferBackendStrategy Strategy = RingBufferBackendStrategy::Mmap>
struct H2ClientConnectMachine;

template<RingBufferBackendStrategy Strategy = RingBufferBackendStrategy::Mmap>
class H2ClientConnectSequence;

template<RingBufferBackendStrategy Strategy = RingBufferBackendStrategy::Mmap>
struct H2RequestMachine;

template<RingBufferBackendStrategy Strategy>
auto buildRequestOperation(H2Client<Strategy>& client, Http2Request&& request);

class CaptureSchedulerAwaitable {
public:
    explicit CaptureSchedulerAwaitable(Scheduler** out) noexcept
        : m_out(out)
    {
    }

    bool await_ready() const noexcept {
        return false;
    }

    template<typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> handle) noexcept {
        if (m_out != nullptr) {
            *m_out = handle.promise().taskRefView().belongScheduler();
        }
        return false;
    }

    void await_resume() const noexcept {}

private:
    Scheduler** m_out = nullptr;
};
} // namespace detail

/**
 * @brief HTTP/2 over TLS 客户端 builder
 * @details 负责收集连接前的协议参数与 TLS 验证策略，不直接建立网络连接。
 */
class H2ClientBuilder {
public:
    H2ClientBuilder& tcpNoDelay(bool v)              { m_config.tcp_no_delay = v; return *this; }
    H2ClientBuilder& maxConcurrentStreams(uint32_t v)  { m_config.max_concurrent_streams = v; return *this; }
    H2ClientBuilder& initialWindowSize(uint32_t v)    { m_config.initial_window_size = v; return *this; }
    H2ClientBuilder& maxFrameSize(uint32_t v)         { m_config.max_frame_size = v; return *this; }
    H2ClientBuilder& maxHeaderListSize(uint32_t v)    { m_config.max_header_list_size = v; return *this; }
    H2ClientBuilder& verifyPeer(bool v)               { m_config.verify_peer = v; return *this; }
    H2ClientBuilder& caPath(std::string v)            { m_config.ca_path = std::move(v); return *this; }
    H2Client<> build() const;
    H2ClientConfig buildConfig() const {
        return Http2Conn::normalizeSettingsConfig(m_config);
    }

private:
    H2ClientConfig m_config;
};

/**
 * @brief HTTP/2 over TLS 客户端
 * @details
 * 典型调用顺序：
 * 1. `connect(host, port)` 建立 TCP 连接、执行 TLS 握手并完成 HTTP/2 preface
 * 2. `request(...)` 或 `get()/post()` 创建流并发送请求
 * 3. 读取 `Http2Stream` 上的响应帧/聚合响应
 * 4. `close()`
 *
 * 连接完成语义：
 * - `connect()` 成功返回后，ALPN 已被校验为 `h2`
 * - 内部连接对象和 stream manager 已进入可用状态
 *
 * 所有权说明：
 * - 客户端独占持有 TLS socket 与 `Http2ConnImpl`
 * - 返回给调用方的 `Http2Stream::ptr` 共享持有具体流对象，但连接生命周期仍由客户端控制
 */
template<RingBufferBackendStrategy Strategy>
class H2Client
{
public:
    using ConnectAwaitable = Task<std::expected<bool, Http2ErrorCode>>;

    H2Client(const H2ClientConfig& config = H2ClientConfig());
    ~H2Client() = default;

    H2Client(const H2Client&) = delete;
    H2Client& operator=(const H2Client&) = delete;
    H2Client(H2Client&&) noexcept = default;
    H2Client& operator=(H2Client&&) noexcept = default;

    /**
     * @brief 建立到目标主机的 h2 连接
     * @param host 目标主机名，会同时用于 TCP 连接与 TLS SNI / authority 语义
     * @param port 目标端口，默认 443
     * @return `Task<std::expected<bool, Http2ErrorCode>>`；成功后连接进入可发起 stream 的状态
     */
    ConnectAwaitable connect(const std::string& host, uint16_t port = 443);

    /**
     * @brief 发送一个完整的 HTTP/2 请求
     * @param request 待发送的请求对象，按值传入并在内部移动消费
     * @return 请求 awaitable；成功后可获得与该请求绑定的 `Http2Stream`
     * @note 调用前必须已经成功完成 `connect()`
     */
    auto request(Http2Request request);

    /**
     * @brief 创建 GET 请求流
     * @param path 请求路径
     * @return 共享流对象，后续可从中读取响应帧与聚合响应
     */
    Http2Stream::ptr get(const std::string& path);

    /**
     * @brief 创建 POST 请求流
     * @param path 请求路径
     * @param body 请求体
     * @param content_type `content-type` 首部值
     * @return 共享流对象，后续可从中读取响应帧与聚合响应
     */
    Http2Stream::ptr post(const std::string& path,
                          const std::string& body,
                          const std::string& content_type = "application/x-www-form-urlencoded");

    /**
     * @brief 优雅关闭连接
     * @return `Task<std::expected<bool, Http2ErrorCode>>`
     * @details 结果反映 GOAWAY / 关闭流程是否成功完成；若连接已损坏，错误会通过 `Http2ErrorCode` 返回
     */
    Task<std::expected<bool, Http2ErrorCode>> close();

    bool isConnected() const { return m_connected; }

    std::string getALPNProtocol() const {
        if (m_socket) {
            return m_socket->getALPNProtocol();
        }
        return m_alpn_protocol;
    }

private:
    template<RingBufferBackendStrategy>
    friend struct detail::H2ClientConnectMachine;
    template<RingBufferBackendStrategy>
    friend class detail::H2ClientConnectSequence;
    template<RingBufferBackendStrategy>
    friend struct detail::H2RequestMachine;
    template<RingBufferBackendStrategy RequestStrategy>
    friend auto detail::buildRequestOperation(
        H2Client<RequestStrategy>& client,
        Http2Request&& request);

    H2ClientConfig m_config;
    std::string m_host;
    std::string m_authority;
    uint16_t m_port = 0;
    bool m_connected = false;
    uint32_t m_next_stream_id = 1;
    std::string m_alpn_protocol;
    Scheduler* m_scheduler = nullptr;

    galay::ssl::SslContext m_ssl_ctx;
    galay::ssl::SslSocket m_dummy_socket{nullptr};
    std::unique_ptr<galay::ssl::SslSocket> m_socket;
    std::unique_ptr<Http2ConnImpl<galay::ssl::SslSocket, Strategy>> m_conn;
    std::expected<bool, Http2ErrorCode> m_connect_result{true};
    std::expected<bool, Http2ErrorCode> m_close_result{true};

    bool ensureActiveStreamManager();
};

namespace detail
{

template<RingBufferBackendStrategy Strategy>
inline bool parseFrameFromRingBuffer(RingBuffer<Strategy>& ring_buffer,
                                     uint32_t max_frame_size,
                                     std::optional<Http2ErrorCode>& error,
                                     std::vector<uint8_t>& scratch,
                                     Http2Frame::uptr& frame)
{
    std::array<iovec, 2> read_iovecs{};
    const size_t iov_count = ring_buffer.getReadIovecs(read_iovecs.data(), read_iovecs.size());
    if (iov_count == 0) {
        return false;
    }

    const size_t available = IoVecBytes::sum(read_iovecs, iov_count);
    if (available < kHttp2FrameHeaderLength) {
        return false;
    }

    uint8_t header_buf[kHttp2FrameHeaderLength];
    if (IoVecBytes::copyPrefix(read_iovecs, iov_count, header_buf, kHttp2FrameHeaderLength)
        < kHttp2FrameHeaderLength) {
        return false;
    }

    const Http2FrameHeader header = Http2FrameHeader::deserialize(header_buf);
    if (header.length > max_frame_size) {
        error = Http2ErrorCode::FrameSizeError;
        return false;
    }

    const size_t frame_size = kHttp2FrameHeaderLength + static_cast<size_t>(header.length);
    if (available < frame_size) {
        return false;
    }

    const iovec* first_segment = IoVecWindow::firstNonEmpty(read_iovecs, iov_count);
    if (first_segment != nullptr && first_segment->iov_len >= frame_size) {
        const auto* frame_data = static_cast<const uint8_t*>(first_segment->iov_base);
        auto frame_result = Http2FrameParser::parseFrame(frame_data, frame_size);
        ring_buffer.consume(frame_size);
        if (!frame_result.has_value()) {
            error = Http2ErrorCode::ProtocolError;
            return false;
        }
        frame = std::move(frame_result.value());
        return true;
    }

    if (scratch.size() < frame_size) {
        scratch.resize(frame_size);
    }

    if (IoVecBytes::copyPrefix(read_iovecs, iov_count, scratch.data(), frame_size) < frame_size) {
        return false;
    }

    auto frame_result = Http2FrameParser::parseFrame(scratch.data(), frame_size);
    ring_buffer.consume(frame_size);
    if (!frame_result.has_value()) {
        error = Http2ErrorCode::ProtocolError;
        return false;
    }
    frame = std::move(frame_result.value());
    return true;
}

template<RingBufferBackendStrategy Strategy>
struct H2ClientConnectMachine {
    using result_type = std::expected<bool, Http2ErrorCode>;

    explicit H2ClientConnectMachine(H2Client<Strategy>& client)
        : m_client(&client)
        , m_server_host(IPType::IPV4, client.m_host, client.m_port)
        , m_driver(client.m_socket.get())
    {
    }

    MachineAction<result_type> advance() {
        if (m_result.has_value()) {
            return MachineAction<result_type>::complete(std::move(*m_result));
        }

        switch (m_phase) {
            case Phase::kConnect:
                return MachineAction<result_type>::waitConnect(m_server_host);
            case Phase::kHandshake:
            case Phase::kSendPreface:
                return advanceSsl();
            case Phase::kDone:
                return MachineAction<result_type>::complete(
                    m_result.value_or(std::unexpected(Http2ErrorCode::ConnectError)));
        }

        fail("internal-fail", "invalid connect phase");
        return MachineAction<result_type>::complete(std::move(*m_result));
    }

    void onConnect(std::expected<void, IOError> result) {
        if (!result) {
            HTTP_LOG_ERROR("[h2] [connect-fail]",
                           "error={}",
                           result.error().message());
            fail();
            return;
        }

        m_phase = Phase::kHandshake;
        m_driver.startHandshake();
    }

    void onRead(std::expected<size_t, IOError> result) {
        m_driver.onRead(std::move(result));
    }

    void onWrite(std::expected<size_t, IOError> result) {
        m_driver.onWrite(std::move(result));
    }

private:
    enum class Phase : uint8_t {
        kConnect,
        kHandshake,
        kSendPreface,
        kDone,
    };

    MachineAction<result_type> advanceSsl() {
        auto wait = m_driver.poll();
        if (m_driver.completed()) {
            if (m_phase == Phase::kHandshake) {
                handleHandshakeResult(m_driver.takeHandshakeResult());
            } else if (m_phase == Phase::kSendPreface) {
                handlePrefaceSendResult(m_driver.takeSendResult());
            } else {
                fail("internal-fail", "ssl driver completed in invalid phase");
            }
            return advance();
        }

        if (wait.kind == galay::ssl::SslOperationDriver::WaitKind::kRead) {
            return MachineAction<result_type>::waitRead(
                m_driver.recvContext().m_buffer,
                m_driver.recvContext().m_length);
        }

        if (wait.kind == galay::ssl::SslOperationDriver::WaitKind::kWrite) {
            return MachineAction<result_type>::waitWrite(
                m_driver.sendContext().m_buffer,
                m_driver.sendContext().m_length);
        }

        fail("internal-fail", "ssl driver returned no wait action");
        return MachineAction<result_type>::complete(std::move(*m_result));
    }

    void handleHandshakeResult(std::expected<void, galay::ssl::SslError> result) {
        if (!result) {
            HTTP_LOG_ERROR("[h2] [handshake-fail]",
                           "error={}",
                           result.error().message());
            fail(Http2Error(result.error()).code());
            return;
        }

        m_client->m_alpn_protocol = m_client->m_socket->getALPNProtocol();
        if (m_client->m_alpn_protocol != "h2") {
            HTTP_LOG_ERROR("[h2] [alpn-fail]",
                           "got={} expect=h2",
                           m_client->m_alpn_protocol);
            fail();
            return;
        }

        preparePreface();
        m_phase = Phase::kSendPreface;
        m_driver.startSend(m_preface.data(), m_preface.size());
    }

    void handlePrefaceSendResult(std::expected<size_t, galay::ssl::SslError> result) {
        if (!result) {
            HTTP_LOG_ERROR("[h2] [preface-send-fail]",
                           "error={}",
                           result.error().message());
            fail(Http2Error(result.error()).code());
            return;
        }

        if (result.value() != m_preface.size()) {
            HTTP_LOG_ERROR("[h2] [preface-send-fail]", "short-write");
            fail();
            return;
        }

        finalizeConnection();
    }

    void preparePreface() {
        m_preface.assign(kHttp2ConnectionPreface.begin(), kHttp2ConnectionPreface.end());
        auto settings = Http2ConnImpl<galay::ssl::SslSocket, Strategy>::makeSettingsFrameFromConfig(
            m_client->m_config,
            0);
        settings.header().stream_id = 0;
        m_preface.append(settings.serialize());
    }

    void finalizeConnection() {
        m_client->m_connected = true;
        m_client->m_connect_result = true;
        m_result = true;
        m_phase = Phase::kDone;
    }

    void fail()
    {
        fail(Http2ErrorCode::ConnectError);
    }

    void fail(const char* phase, const char* detail)
    {
        HTTP_LOG_ERROR("[h2]", "phase={} detail={}", phase, detail);
        fail(Http2ErrorCode::ConnectError);
    }

    void fail(Http2ErrorCode code)
    {
        m_client->m_connected = false;
        m_client->m_connect_result = std::unexpected(code);
        m_result = std::unexpected(code);
        m_phase = Phase::kDone;
    }

    H2Client<Strategy>* m_client = nullptr;
    Host m_server_host;
    galay::ssl::SslOperationDriver m_driver;
    std::string m_preface;
    std::optional<result_type> m_result;
    Phase m_phase = Phase::kConnect;
};

template<RingBufferBackendStrategy Strategy>
class H2ClientConnectSequence
    : public SequenceAwaitableBase
    , public TimeoutSupport<H2ClientConnectSequence<Strategy>>
{
public:
    using ResultType = std::expected<bool, Http2ErrorCode>;
    using result_type = ResultType;

    H2ClientConnectSequence(const H2ClientConnectSequence&) = delete;
    H2ClientConnectSequence& operator=(const H2ClientConnectSequence&) = delete;
    H2ClientConnectSequence(H2ClientConnectSequence&&) noexcept = default;
    H2ClientConnectSequence& operator=(H2ClientConnectSequence&&) noexcept = default;

private:
    static void discardSocket(std::unique_ptr<galay::ssl::SslSocket>& socket) {
        if (socket && socket->handle().fd >= 0) {
            int close_result = ::close(socket->handle().fd);
            if (close_result != 0) {
                HTTP_LOG_WARN("[h2-client] [socket-close-fail]", "discard socket");
            }
        }
        socket.reset();
    }

    static void discardTransport(H2Client<Strategy>& client) {
        if (client.m_conn) {
            if (client.m_conn->socket().handle().fd >= 0) {
                int close_result = ::close(client.m_conn->socket().handle().fd);
                if (close_result != 0) {
                    client.m_close_result = std::unexpected(Http2ErrorCode::ConnectError);
                }
            }
            client.m_conn.reset();
        }
        discardSocket(client.m_socket);
        client.m_connected = false;
        client.m_scheduler = nullptr;
    }

    static bool finalizeTransport(H2Client<Strategy>& client) {
        if (client.m_socket == nullptr) {
            return false;
        }

        client.m_conn =
            std::make_unique<Http2ConnImpl<galay::ssl::SslSocket, Strategy>>(std::move(*client.m_socket));
        client.m_socket.reset();
        client.m_conn->setIsClient(true);
        auto local_settings = Http2ConnImpl<galay::ssl::SslSocket, Strategy>::makeSettingsFrameFromConfig(
            client.m_config,
            0);
        if (client.m_conn->applyLocalSettings(local_settings) != Http2ErrorCode::NoError) {
            client.m_conn.reset();
            return false;
        }
        client.m_conn->markSettingsSent();
        client.m_connected = true;
        client.m_connect_result = true;
        HTTP_LOG_INFO("[connect] [h2]", "host={} port={}", client.m_host, client.m_port);
        return true;
    }

public:
    H2ClientConnectSequence(H2Client<Strategy>& client, std::string host, uint16_t port)
        : SequenceAwaitableBase(client.m_dummy_socket.controller())
        , m_client(&client)
        , m_result(true)
    {
        m_client->m_connected = false;
        m_client->m_host = std::move(host);
        m_client->m_port = port;
        m_client->m_authority = m_client->m_host + ":" + std::to_string(m_client->m_port);
        m_client->m_alpn_protocol.clear();
        m_client->m_scheduler = nullptr;
        m_client->m_connect_result = std::unexpected(Http2ErrorCode::ConnectError);
        m_client->m_close_result = true;
        m_client->m_conn.reset();
        m_client->m_socket = std::make_unique<galay::ssl::SslSocket>(&m_client->m_ssl_ctx);
        m_controller = m_client->m_socket->controller();

        auto nonblock_result = m_client->m_socket->option().handleNonBlock();
        if (!nonblock_result) {
            HTTP_LOG_ERROR("[h2] [connect] [nonblock-fail]",
                           "error={}",
                           nonblock_result.error().message());
            discardTransport(*m_client);
            m_ready = true;
            return;
        }
        if (m_client->m_config.tcp_no_delay) {
            auto nodelay_result = m_client->m_socket->option().handleTcpNoDelay();
            if (!nodelay_result) {
                HTTP_LOG_ERROR("[h2] [connect] [nodelay-fail]",
                               "error={}",
                               nodelay_result.error().message());
                discardTransport(*m_client);
                m_ready = true;
                return;
            }
        }

        auto sni_result = m_client->m_socket->setHostname(m_client->m_host);
        if (!sni_result) {
            HTTP_LOG_ERROR("[h2] [sni-fail]",
                           "error={}",
                           sni_result.error().message());
            discardTransport(*m_client);
            m_ready = true;
            return;
        }

        m_inner_operation = std::make_unique<InnerOperation>(
            AwaitableBuilder<ResultType>::fromStateMachine(
                m_client->m_socket->controller(),
                H2ClientConnectMachine<Strategy>(*m_client))
                .build()
        );
    }

    ~H2ClientConnectSequence() {
        cleanupInnerIfArmed();
    }

    bool await_ready() const noexcept {
        return m_ready || (m_inner_operation != nullptr && m_inner_operation->await_ready());
    }

    template<typename Promise>
    decltype(auto) await_suspend(std::coroutine_handle<Promise> handle) {
        if (m_inner_operation == nullptr) {
            return false;
        }
        m_client->m_scheduler = handle.promise().taskRefView().belongScheduler();
        m_inner_armed = true;
        return m_inner_operation->await_suspend(handle);
    }

    ResultType await_resume() {
        if (!m_result.has_value()) {
            cleanupInnerIfArmed();
            discardTransport(*m_client);
            m_client->m_connect_result = std::unexpected(Http2ErrorCode::ConnectError);
            return std::unexpected(Http2ErrorCode::ConnectError);
        }

        if (m_ready) {
            return std::unexpected(Http2ErrorCode::ConnectError);
        }

        auto result = resumeInner();
        if (!result) {
            discardTransport(*m_client);
            m_client->m_connect_result = std::unexpected(result.error());
            return result;
        }
        if (!finalizeTransport(*m_client)) {
            discardTransport(*m_client);
            m_client->m_connect_result = std::unexpected(Http2ErrorCode::ConnectError);
            return std::unexpected(Http2ErrorCode::ConnectError);
        }
        return result;
    }

    IOTask* front() override {
        return m_inner_operation ? m_inner_operation->front() : nullptr;
    }

    const IOTask* front() const override {
        return m_inner_operation ? m_inner_operation->front() : nullptr;
    }

    void popFront() override {
        if (m_inner_operation) {
            m_inner_operation->popFront();
        }
    }

    bool empty() const override {
        return m_inner_operation == nullptr || m_inner_operation->empty();
    }

#ifdef USE_IOURING
    SequenceProgress prepareForSubmit() override {
        return m_inner_operation ? m_inner_operation->prepareForSubmit() : SequenceProgress::kCompleted;
    }

    SequenceProgress onActiveEvent(struct io_uring_cqe* cqe, GHandle handle) override {
        return m_inner_operation ? m_inner_operation->onActiveEvent(cqe, handle) : SequenceProgress::kCompleted;
    }
#else
    SequenceProgress prepareForSubmit(GHandle handle) override {
        return m_inner_operation ? m_inner_operation->prepareForSubmit(handle) : SequenceProgress::kCompleted;
    }

    SequenceProgress onActiveEvent(GHandle handle) override {
        return m_inner_operation ? m_inner_operation->onActiveEvent(handle) : SequenceProgress::kCompleted;
    }
#endif

public:
    std::expected<bool, IOError> m_result;

private:
    using InnerOperation = galay::kernel::StateMachineAwaitable<H2ClientConnectMachine<Strategy>>;

    ResultType resumeInner() {
        m_inner_completed = true;
        return m_inner_operation->await_resume();
    }

    void cleanupInnerIfArmed() {
        if (m_inner_operation != nullptr && m_inner_armed && !m_inner_completed) {
            m_inner_operation->onCompleted();
            m_inner_completed = true;
        }
    }

    H2Client<Strategy>* m_client = nullptr;
    std::unique_ptr<InnerOperation> m_inner_operation;
    bool m_ready = false;
    bool m_inner_armed = false;
    bool m_inner_completed = false;
};

template<RingBufferBackendStrategy Strategy>
struct H2RequestMachine {
    using result_type = std::expected<std::optional<Http2Response>, Http2ErrorCode>;

    H2RequestMachine(H2Client<Strategy>* client, Http2Request request)
        : m_client(client)
        , m_request(std::move(request))
    {
    }

    galay::ssl::SslMachineAction<result_type> advance() {
        if (!m_initialized) {
            initialize();
        }

        if (m_result.has_value()) {
            return galay::ssl::SslMachineAction<result_type>::complete(std::move(*m_result));
        }

        if (m_error.has_value()) {
            m_result = std::unexpected(*m_error);
            return galay::ssl::SslMachineAction<result_type>::complete(std::move(*m_result));
        }

        if (hasPendingControlSend()) {
            return galay::ssl::SslMachineAction<result_type>::send(
                m_control_send_buffer.data() + m_control_send_offset,
                m_control_send_buffer.size() - m_control_send_offset);
        }

        if (m_send_offset < m_send_buffer.size()) {
            return galay::ssl::SslMachineAction<result_type>::send(
                m_send_buffer.data() + m_send_offset,
                m_send_buffer.size() - m_send_offset);
        }

        if (consumeResponseFrames()) {
            if (m_result.has_value()) {
                return galay::ssl::SslMachineAction<result_type>::complete(std::move(*m_result));
            }
            if (m_error.has_value()) {
                m_result = std::unexpected(*m_error);
                return galay::ssl::SslMachineAction<result_type>::complete(std::move(*m_result));
            }
        }

        char* recv_buffer = nullptr;
        size_t recv_length = 0;
        if (!prepareRecvWindow(recv_buffer, recv_length)) {
            m_error = Http2ErrorCode::InternalError;
            m_result = std::unexpected(*m_error);
            return galay::ssl::SslMachineAction<result_type>::complete(std::move(*m_result));
        }

        return galay::ssl::SslMachineAction<result_type>::recv(recv_buffer, recv_length);
    }

    void onHandshake(std::expected<void, galay::ssl::SslError>) {}

    void onRecv(std::expected<Bytes, galay::ssl::SslError> result) {
        if (!result) {
            setSslRecvError(result.error());
            m_result = std::unexpected(*m_error);
            return;
        }

        const size_t bytes_read = result.value().size();
        if (bytes_read == 0) {
            m_error = Http2ErrorCode::ConnectError;
            m_result = std::unexpected(*m_error);
            return;
        }

        m_client->m_conn->ringBuffer().produce(bytes_read);
        if (consumeResponseFrames() && !m_result.has_value() && m_error.has_value()) {
            m_result = std::unexpected(*m_error);
        }
    }

    void onSend(std::expected<size_t, galay::ssl::SslError> result) {
        if (!result) {
            setSslSendError(result.error());
            m_result = std::unexpected(*m_error);
            return;
        }

        if (result.value() == 0) {
            m_error = Http2ErrorCode::InternalError;
            m_result = std::unexpected(*m_error);
            return;
        }

        if (hasPendingControlSend()) {
            m_control_send_offset += result.value();
            if (m_control_send_offset > m_control_send_buffer.size()) {
                m_error = Http2ErrorCode::InternalError;
                m_result = std::unexpected(*m_error);
                return;
            }
            if (m_control_send_offset == m_control_send_buffer.size()) {
                m_control_send_buffer.clear();
                m_control_send_offset = 0;
            }
            return;
        }

        m_send_offset += result.value();
        if (m_send_offset > m_send_buffer.size()) {
            m_error = Http2ErrorCode::InternalError;
            m_result = std::unexpected(*m_error);
        }
    }

    void onShutdown(std::expected<void, galay::ssl::SslError>) {}

private:
    void initialize() {
        m_initialized = true;

        if (!m_client->m_connected || !m_client->m_conn) {
            m_error = Http2ErrorCode::ConnectError;
            return;
        }

        if (m_client->m_conn->isDraining() || m_client->m_conn->isGoawayReceived()) {
            m_error = Http2ErrorCode::RefusedStream;
            return;
        }

        m_stream_id = m_client->m_next_stream_id;
        m_client->m_next_stream_id += 2;
        m_client->m_conn->createStream(m_stream_id);

        std::vector<Http2HeaderField> headers;
        headers.push_back({":method", m_request.method});
        headers.push_back({":scheme", m_request.scheme});
        headers.push_back({":authority", m_request.authority});
        headers.push_back({":path", m_request.path.empty() ? "/" : m_request.path});
        for (const auto& header : m_request.headers) {
            headers.push_back(header);
        }

        auto headers_frame = Http2FrameBuilder::headers(
            m_stream_id,
            m_client->m_conn->encoder().encode(headers),
            m_request.body.empty(),
            true);
        m_send_buffer = headers_frame->serialize();

        if (!m_request.body.empty()) {
            auto data_frame = Http2FrameBuilder::data(
                m_stream_id,
                m_request.takeCoalescedBody(),
                true);
            m_send_buffer.append(data_frame->serialize());
        }
    }

    bool prepareRecvWindow(char*& buffer, size_t& length) {
        auto write_iovecs = borrowWriteIovecs(m_client->m_conn->ringBuffer());
        if (!IoVecWindow::bindFirstNonEmpty(write_iovecs, buffer, length)) {
            buffer = nullptr;
            length = 0;
            return false;
        }
        return length > 0;
    }

    void setSslSendError(const galay::ssl::SslError& error) {
        HTTP_LOG_ERROR("[h2] [send-fail]", "error={}", error.message());
        m_error = Http2Error(error).code();
    }

    void setSslRecvError(const galay::ssl::SslError& error) {
        HTTP_LOG_ERROR("[h2] [recv-fail]", "error={}", error.message());
        m_error = Http2Error(error).code();
    }

    bool parseNextFrame(Http2Frame::uptr& frame) {
        return parseFrameFromRingBuffer(
            m_client->m_conn->ringBuffer(),
            m_client->m_conn->peerSettings().max_frame_size,
            m_error,
            m_frame_scratch,
            frame);
    }

    bool hasPendingControlSend() const {
        return m_control_send_offset < m_control_send_buffer.size();
    }

    void scheduleSettingsAck() {
        if (hasPendingControlSend()) {
            return;
        }
        Http2SettingsFrame ack;
        ack.setAck(true);
        m_control_send_buffer = ack.serialize();
        m_control_send_offset = 0;
    }

    bool decodeHeaderBlock() {
        auto decode_result = m_client->m_conn->decoder().decode(m_header_block);
        if (!decode_result.has_value()) {
            m_error = Http2ErrorCode::CompressionError;
            return false;
        }

        for (const auto& field : decode_result.value()) {
            if (field.name == ":status") {
                int status = 0;
                const char* begin = field.value.data();
                const char* end = begin + field.value.size();
                auto [ptr, ec] = std::from_chars(begin, end, status);
                if (ec != std::errc{} || ptr != end) {
                    m_error = Http2ErrorCode::ProtocolError;
                    return false;
                }
                m_response_data.status = status;
            } else {
                m_response_data.headers.push_back(field);
            }
        }

        m_header_block.clear();
        return true;
    }

    bool consumeResponseFrames() {
        if (m_error.has_value() || m_result.has_value()) {
            return true;
        }

        while (true) {
            Http2Frame::uptr frame;
            if (!parseNextFrame(frame)) {
                return m_error.has_value() || m_result.has_value();
            }

            if (!frame) {
                continue;
            }

            switch (frame->type()) {
                case Http2FrameType::Settings: {
                    auto* settings = static_cast<Http2SettingsFrame*>(frame.get());
                    if (settings->isAck()) {
                        m_client->m_conn->markSettingsAckReceived();
                    } else {
                        auto error = m_client->m_conn->applyPeerSettings(*settings);
                        if (error != Http2ErrorCode::NoError) {
                            m_error = error;
                            return true;
                        }
                        scheduleSettingsAck();
                    }
                    continue;
                }
                case Http2FrameType::Headers: {
                    auto* headers = static_cast<Http2HeadersFrame*>(frame.get());
                    if (frame->streamId() != m_stream_id) {
                        continue;
                    }

                    m_header_block.append(headers->headerBlock());
                    if (headers->isEndHeaders()) {
                        if (!decodeHeaderBlock()) {
                            return true;
                        }
                        if (headers->isEndStream()) {
                            m_result = std::optional<Http2Response>(std::move(m_response_data));
                            return true;
                        }
                    }
                    continue;
                }
                case Http2FrameType::Continuation: {
                    auto* continuation = static_cast<Http2ContinuationFrame*>(frame.get());
                    if (frame->streamId() != m_stream_id) {
                        continue;
                    }

                    m_header_block.append(continuation->headerBlock());
                    if (continuation->isEndHeaders()) {
                        if (!decodeHeaderBlock()) {
                            return true;
                        }
                    }
                    continue;
                }
                case Http2FrameType::Data: {
                    auto* data = static_cast<Http2DataFrame*>(frame.get());
                    if (frame->streamId() != m_stream_id) {
                        continue;
                    }

                    m_response_data.body.append(data->data());
                    if (data->isEndStream()) {
                        m_result = std::optional<Http2Response>(std::move(m_response_data));
                        return true;
                    }
                    continue;
                }
                case Http2FrameType::GoAway:
                    if (auto* goaway = frame->asGoAway()) {
                        m_client->m_conn->markGoawayReceived(
                            goaway->lastStreamId(),
                            goaway->errorCode(),
                            goaway->debugData());
                        m_error = (m_stream_id > goaway->lastStreamId())
                            ? Http2ErrorCode::RefusedStream
                            : Http2ErrorCode::ProtocolError;
                    } else {
                        m_error = Http2ErrorCode::ProtocolError;
                    }
                    return true;
                case Http2FrameType::RstStream:
                    if (frame->streamId() == m_stream_id) {
                        m_error = Http2ErrorCode::StreamClosed;
                        return true;
                    }
                    continue;
                default:
                    continue;
            }
        }
    }

    H2Client<Strategy>* m_client = nullptr;
    Http2Request m_request;
    std::string m_send_buffer;
    size_t m_send_offset = 0;
    std::string m_control_send_buffer;
    size_t m_control_send_offset = 0;
    std::string m_header_block;
    Http2Response m_response_data;
    uint32_t m_stream_id = 0;
    std::vector<uint8_t> m_frame_scratch;
    std::optional<Http2ErrorCode> m_error;
    std::optional<result_type> m_result;
    bool m_initialized = false;
};

template<RingBufferBackendStrategy Strategy>
inline auto buildRequestOperation(H2Client<Strategy>& client, Http2Request&& request) {
    using ResultType = typename H2RequestMachine<Strategy>::result_type;
    auto machine = H2RequestMachine<Strategy>(&client, std::move(request));
    return galay::ssl::SslAwaitableBuilder<ResultType>::fromStateMachine(
               client.m_conn ? client.m_conn->socket().controller() : client.m_dummy_socket.controller(),
               client.m_conn ? &client.m_conn->socket() : &client.m_dummy_socket,
               std::move(machine))
        .build();
}

} // namespace detail

template<RingBufferBackendStrategy Strategy>
inline H2Client<Strategy>::H2Client(const H2ClientConfig& config)
    : m_config(Http2ConnImpl<galay::ssl::SslSocket, Strategy>::normalizeSettingsConfig(config))
    , m_connected(false)
    , m_next_stream_id(1)
    , m_ssl_ctx(galay::ssl::SslMethod::TLS_Client)
{
    m_ssl_ctx.setALPNProtocols({"h2"});

    if (config.verify_peer) {
        m_ssl_ctx.setVerifyMode(galay::ssl::SslVerifyMode::Peer);
        if (!config.ca_path.empty()) {
            m_ssl_ctx.loadCACertificate(config.ca_path);
        } else {
            m_ssl_ctx.useDefaultCA();
        }
    } else {
        m_ssl_ctx.setVerifyMode(galay::ssl::SslVerifyMode::None);
    }
}

template<RingBufferBackendStrategy Strategy>
inline typename H2Client<Strategy>::ConnectAwaitable H2Client<Strategy>::connect(
    const std::string& host,
    uint16_t port) {
    const auto discard_socket = [](std::unique_ptr<galay::ssl::SslSocket>& socket) {
        if (socket && socket->handle().fd >= 0) {
            int close_result = ::close(socket->handle().fd);
            if (close_result != 0) {
                HTTP_LOG_WARN("[h2-client] [socket-close-fail]", "discard socket");
            }
        }
        socket.reset();
    };

    const auto discard_transport = [this, &discard_socket]() {
        if (m_conn) {
            if (m_conn->socket().handle().fd >= 0) {
                int close_result = ::close(m_conn->socket().handle().fd);
                if (close_result != 0) {
                    m_close_result = std::unexpected(Http2ErrorCode::ConnectError);
                }
            }
            m_conn.reset();
        }
        discard_socket(m_socket);
        m_connected = false;
        m_scheduler = nullptr;
    };

    const auto finalize_transport = [this]() -> bool {
        if (m_socket == nullptr) {
            return false;
        }

        m_conn = std::make_unique<Http2ConnImpl<galay::ssl::SslSocket, Strategy>>(std::move(*m_socket));
        m_socket.reset();
        m_conn->setIsClient(true);
        auto local_settings = Http2ConnImpl<galay::ssl::SslSocket, Strategy>::makeSettingsFrameFromConfig(
            m_config,
            0);
        if (m_conn->applyLocalSettings(local_settings) != Http2ErrorCode::NoError) {
            m_conn.reset();
            return false;
        }
        m_conn->markSettingsSent();
        m_connected = true;
        m_connect_result = true;
        return true;
    };

    m_connected = false;
    m_host = host;
    m_port = port;
    m_authority = m_host + ":" + std::to_string(m_port);
    m_alpn_protocol.clear();
    m_scheduler = nullptr;
    m_connect_result = std::unexpected(Http2ErrorCode::ConnectError);
    m_close_result = true;
    m_conn.reset();
    m_socket = std::make_unique<galay::ssl::SslSocket>(&m_ssl_ctx);

    co_await detail::CaptureSchedulerAwaitable(&m_scheduler);
    if (m_scheduler == nullptr) {
        HTTP_LOG_ERROR("[h2] [connect]", "scheduler missing");
        discard_transport();
        co_return std::unexpected(Http2ErrorCode::ConnectError);
    }

    auto nonblock_result = m_socket->option().handleNonBlock();
    if (!nonblock_result) {
        HTTP_LOG_ERROR("[h2] [connect] [nonblock-fail]",
                       "error={}",
                       nonblock_result.error().message());
        discard_transport();
        co_return std::unexpected(Http2ErrorCode::ConnectError);
    }
    if (m_config.tcp_no_delay) {
        auto nodelay_result = m_socket->option().handleTcpNoDelay();
        if (!nodelay_result) {
            HTTP_LOG_ERROR("[h2] [connect] [nodelay-fail]",
                           "error={}",
                           nodelay_result.error().message());
            discard_transport();
            co_return std::unexpected(Http2ErrorCode::ConnectError);
        }
    }

    auto sni_result = m_socket->setHostname(m_host);
    if (!sni_result) {
        HTTP_LOG_ERROR("[h2] [sni-fail]", "error={}", sni_result.error().message());
        discard_transport();
        co_return std::unexpected(Http2ErrorCode::ConnectError);
    }

    Host server_host(IPType::IPV4, m_host, m_port);
    auto connect_result = co_await m_socket->connect(server_host);
    if (!connect_result) {
        HTTP_LOG_ERROR("[h2] [connect-fail]",
                       "error={}",
                       connect_result.error().message());
        discard_transport();
        co_return std::unexpected(Http2ErrorCode::ConnectError);
    }

    auto handshake_result = co_await m_socket->handshake();
    if (!handshake_result) {
        HTTP_LOG_ERROR("[h2] [handshake-fail]",
                       "error={}",
                       handshake_result.error().message());
        discard_transport();
        co_return std::unexpected(Http2ErrorCode::ConnectError);
    }

    m_alpn_protocol = m_socket->getALPNProtocol();
    if (m_alpn_protocol != "h2") {
        HTTP_LOG_ERROR("[h2] [alpn-fail]", "got={} expect=h2", m_alpn_protocol);
        discard_transport();
        co_return std::unexpected(Http2ErrorCode::ConnectError);
    }

    std::string preface(kHttp2ConnectionPreface.begin(), kHttp2ConnectionPreface.end());
    auto settings = Http2ConnImpl<galay::ssl::SslSocket, Strategy>::makeSettingsFrameFromConfig(
        m_config,
        0);
    settings.header().stream_id = 0;
    preface.append(settings.serialize());

    size_t offset = 0;
    while (offset < preface.size()) {
        auto send_result = co_await m_socket->send(preface.data() + offset, preface.size() - offset);
        if (!send_result) {
            HTTP_LOG_ERROR("[h2] [preface-send-fail]",
                           "error={}",
                           send_result.error().message());
            discard_transport();
            co_return std::unexpected(Http2ErrorCode::ConnectError);
        }
        if (send_result.value() == 0) {
            HTTP_LOG_ERROR("[h2] [preface-send-fail]", "short-write");
            discard_transport();
            co_return std::unexpected(Http2ErrorCode::ConnectError);
        }
        offset += send_result.value();
    }

    if (!finalize_transport()) {
        discard_transport();
        co_return std::unexpected(Http2ErrorCode::ConnectError);
    }
    HTTP_LOG_INFO("[connect] [h2]", "host={} port={}", m_host, m_port);

    co_return std::expected<bool, Http2ErrorCode>(true);
}

template<RingBufferBackendStrategy Strategy>
inline auto H2Client<Strategy>::request(Http2Request request) {
    return detail::buildRequestOperation(*this, std::move(request));
}

template<RingBufferBackendStrategy Strategy>
inline bool H2Client<Strategy>::ensureActiveStreamManager() {
    if (!m_connected || !m_conn) {
        return false;
    }

    if (m_conn->streamManager()) {
        return true;
    }

    if (m_scheduler == nullptr) {
        HTTP_LOG_ERROR("[h2] [stream-manager]", "scheduler missing");
        return false;
    }

    m_conn->localSettings().from(m_config);
    m_conn->runtimeConfig().from(m_config);
    m_conn->initStreamManager();

    auto* manager = m_conn->streamManager();
    if (manager == nullptr) {
        HTTP_LOG_ERROR("[h2] [stream-manager]", "init failed");
        return false;
    }

    if (!manager->startWithScheduler(
            m_scheduler,
            [](Http2Stream::ptr) -> galay::kernel::Task<void> { co_return; })) {
        HTTP_LOG_ERROR("[h2] [stream-manager]", "schedule failed");
        return false;
    }
    return true;
}

template<RingBufferBackendStrategy Strategy>
inline Http2Stream::ptr H2Client<Strategy>::get(const std::string& path) {
    if (!ensureActiveStreamManager()) {
        HTTP_LOG_ERROR("[h2] [get]", "client not ready");
        return nullptr;
    }

    auto* manager = m_conn->streamManager();
    auto stream = manager->allocateStream();
    std::vector<Http2HeaderField> headers;
    headers.reserve(4);
    headers.emplace_back(":method", "GET");
    headers.emplace_back(":scheme", "https");
    headers.emplace_back(":authority", m_authority);
    headers.emplace_back(":path", path.empty() ? "/" : path);
    stream->sendHeaders(headers, true);
    return stream;
}

template<RingBufferBackendStrategy Strategy>
inline Http2Stream::ptr H2Client<Strategy>::post(const std::string& path,
                                                 const std::string& body,
                                                 const std::string& content_type) {
    if (!ensureActiveStreamManager()) {
        HTTP_LOG_ERROR("[h2] [post]", "client not ready");
        return nullptr;
    }

    auto* manager = m_conn->streamManager();
    auto stream = manager->allocateStream();
    std::vector<Http2HeaderField> headers;
    headers.reserve(5);
    headers.emplace_back(":method", "POST");
    headers.emplace_back(":scheme", "https");
    headers.emplace_back(":authority", m_authority);
    headers.emplace_back(":path", path.empty() ? "/" : path);
    headers.emplace_back("content-type", content_type);
    stream->sendHeaders(headers, false);
    stream->sendData(body, true);
    return stream;
}

template<RingBufferBackendStrategy Strategy>
inline Task<std::expected<bool, Http2ErrorCode>> H2Client<Strategy>::close() {
    m_connected = false;
    m_close_result = true;

    if (m_conn) {
        if (auto* manager = m_conn->streamManager()) {
            co_await manager->shutdown(Http2ErrorCode::NoError);
        } else {
            auto close_result = co_await m_conn->close();
            if (!close_result) {
                m_close_result = std::unexpected(Http2ErrorCode::ConnectError);
            }
        }
    } else if (m_socket) {
        auto close_result = co_await m_socket->close();
        if (!close_result) {
            m_close_result = std::unexpected(Http2ErrorCode::ConnectError);
        }
    }

    m_conn.reset();
    m_socket.reset();
    co_return m_close_result;
}

inline H2Client<> H2ClientBuilder::build() const {
    return H2Client<>(Http2Conn::normalizeSettingsConfig(m_config));
}

#endif // GALAY_SSL_FEATURE_ENABLED

} // namespace galay::http2

#endif // GALAY_H2_CLIENT_H
