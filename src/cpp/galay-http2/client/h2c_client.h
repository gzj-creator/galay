/**
 * @file h2c_client.h
 * @brief HTTP/2 明文（h2c）客户端，支持 HTTP/1.1 到 HTTP/2 的协议升级
 * @author galay-http
 * @version 1.0.0
 *
 * @details 提供 H2cClient 模板类，支持通过 HTTP/1.1 Upgrade 机制
 *          将明文连接升级为 HTTP/2 连接。包含升级状态机和前置请求/响应处理。
 */

#ifndef GALAY_H2C_CLIENT_H
#define GALAY_H2C_CLIENT_H

#include "../kernel/http2_conn.h"
#include "../kernel/http2_stream.h"
#include "../kernel/stream_manager.h"
#include "../../galay-http/common/iovec_utils.h"
#include "../../galay-http/protoc/http_request.h"
#include "../../galay-http/protoc/http_response.h"
#include "../../galay-http/builder/http_builder.h"
#include "../../galay-kernel/async/tcp_socket.h"
#include "../../galay-utils/cache/ring_buffer.hpp"
#include "../../galay-kernel/core/awaitable.h"
#include "../../galay-kernel/core/task.h"
#include "../../galay-kernel/core/timeout.hpp"
#include "../../galay-utils/encoding/base64.hpp"
#include <memory>
#include <array>
#include <algorithm>
#include <cerrno>
#include <string>
#include <cstring>
#include <optional>
#include <span>

namespace galay::http2
{

using namespace galay::kernel;
using namespace galay::http;
using namespace galay::async;
using ::galay::utils::RingBuffer;

struct H2cClientConfig
{
    uint32_t max_concurrent_streams = 100;
    uint32_t initial_window_size = 65535;
    uint32_t max_frame_size = 16384;
    uint32_t max_header_list_size = 8192;
    bool ping_enabled = true;
    std::chrono::milliseconds ping_interval{30000};
    std::chrono::milliseconds ping_timeout{10000};
    std::chrono::milliseconds settings_ack_timeout{10000};
    std::chrono::milliseconds graceful_shutdown_rtt{100};
    std::chrono::milliseconds graceful_shutdown_timeout{5000};
    uint32_t flow_control_target_window = kDefaultInitialWindowSize;
    Http2FlowControlStrategy flow_control_strategy;
};

class H2cClient;

class H2cClientBuilder {
public:
    H2cClientBuilder& maxConcurrentStreams(uint32_t v)  { m_config.max_concurrent_streams = v; return *this; }
    H2cClientBuilder& initialWindowSize(uint32_t v)    { m_config.initial_window_size = v; return *this; }
    H2cClientBuilder& maxFrameSize(uint32_t v)         { m_config.max_frame_size = v; return *this; }
    H2cClientBuilder& maxHeaderListSize(uint32_t v)    { m_config.max_header_list_size = v; return *this; }
    H2cClientBuilder& pingEnabled(bool v)              { m_config.ping_enabled = v; return *this; }
    H2cClientBuilder& pingInterval(std::chrono::milliseconds v) { m_config.ping_interval = v; return *this; }
    H2cClientBuilder& pingTimeout(std::chrono::milliseconds v) { m_config.ping_timeout = v; return *this; }
    H2cClientBuilder& settingsAckTimeout(std::chrono::milliseconds v) { m_config.settings_ack_timeout = v; return *this; }
    H2cClientBuilder& gracefulShutdownRtt(std::chrono::milliseconds v) { m_config.graceful_shutdown_rtt = v; return *this; }
    H2cClientBuilder& gracefulShutdownTimeout(std::chrono::milliseconds v) { m_config.graceful_shutdown_timeout = v; return *this; }
    H2cClientBuilder& flowControlTargetWindow(uint32_t v) { m_config.flow_control_target_window = v; return *this; }
    H2cClientBuilder& flowControlStrategy(Http2FlowControlStrategy v) {
        m_config.flow_control_strategy = std::move(v);
        return *this;
    }
    H2cClient build() const;
    H2cClientConfig buildConfig() const { return m_config; }
private:
    H2cClientConfig m_config;
};

// Forward declarations
class H2cUpgradeAwaitable;

/**
 * @brief H2c 客户端 (HTTP/2 over cleartext)
 */
class H2cClient
{
public:
    H2cClient(const H2cClientConfig& config = H2cClientConfig(), size_t ring_buffer_size = 65536)
        : m_config(config), m_ring_buffer_size(ring_buffer_size), m_port(0), m_upgraded(false) {}

    ~H2cClient() = default;
    H2cClient(const H2cClient&) = delete;
    H2cClient& operator=(const H2cClient&) = delete;
    H2cClient(H2cClient&&) noexcept = default;
    H2cClient& operator=(H2cClient&&) noexcept = default;

    auto connect(const std::string& host, uint16_t port) {
        return connectImpl(host, port);
    }

    Task<std::expected<void, IOError>> connectImpl(const std::string& host, uint16_t port) {
        m_host = host;
        m_port = port;
        m_authority = m_host + ":" + std::to_string(m_port);
        try {
            m_socket = std::make_unique<TcpSocket>(IPType::IPV4);
            m_ring_buffer = std::make_unique<RingBuffer>(m_ring_buffer_size);
        } catch (...) {
            m_socket.reset();
            m_ring_buffer.reset();
            co_return std::unexpected(IOError(kOpenFailed, errno));
        }
        auto r = m_socket->option().handleNonBlock();
        if (!r) {
            m_socket.reset();
            m_ring_buffer.reset();
            co_return std::unexpected(r.error());
        }
        Host server_host(IPType::IPV4, host, port);
        co_return co_await m_socket->connect(server_host);
    }

    H2cUpgradeAwaitable upgrade(const std::string& path = "/");
    Http2Stream::ptr get(const std::string& path);
    Http2Stream::ptr post(const std::string& path,
                          const std::string& body,
                          const std::string& content_type = "application/x-www-form-urlencoded");
    Task<std::expected<bool, Http2Error>> shutdown();

    bool isUpgraded() const { return m_upgraded; }
    Http2ConnImpl<TcpSocket>* getConn() { return m_conn.get(); }

private:
    friend struct H2cUpgradeMachine;
    friend class H2cUpgradeAwaitable;

    H2cClientConfig m_config;
    std::string m_host;
    std::string m_authority;
    uint16_t m_port;
    size_t m_ring_buffer_size;
    std::unique_ptr<TcpSocket> m_socket;
    std::unique_ptr<RingBuffer> m_ring_buffer;
    std::unique_ptr<Http2ConnImpl<TcpSocket>> m_conn;
    bool m_upgraded;
    std::expected<bool, Http2Error> m_upgrade_result{true};
    std::expected<bool, Http2Error> m_shutdown_result{true};
};

/**
 * @brief H2c 升级状态机
 * @details IO 顺序：SEND upgrade → RECV 101 → SEND preface+settings → RECV settings → SEND ACK
 */
struct H2cUpgradeMachine {
    using result_type = std::expected<bool, Http2Error>;

    explicit H2cUpgradeMachine(H2cClient& client, const std::string& path)
        : m_client(&client)
        , m_ring_buffer(client.m_ring_buffer.get())
    {
        if (client.m_socket == nullptr || m_ring_buffer == nullptr) {
            setConnectError("not connected");
            return;
        }

        prepareUpgradeRequest(path);
        preparePrefaceAndSettings();
        prepareAck();
    }

    MachineAction<result_type> advance() {
        if (m_result.has_value()) {
            return MachineAction<result_type>::complete(std::move(*m_result));
        }

        for (;;) {
            switch (m_phase) {
            case Phase::kSendUpgrade:
                if (sendCompleted(m_upgrade_request_buf, Phase::kRecvUpgradeResponse)) {
                    continue;
                }
                return waitSend(m_upgrade_request_buf);
            case Phase::kRecvUpgradeResponse:
                if (parseUpgradeResponse()) {
                    if (m_result.has_value()) {
                        return MachineAction<result_type>::complete(std::move(*m_result));
                    }
                    m_phase = Phase::kSendPrefaceSettings;
                    continue;
                }
                if (!prepareRecvWindow("RingBuffer full while waiting 101")) {
                    return MachineAction<result_type>::complete(std::move(*m_result));
                }
                return MachineAction<result_type>::waitReadv(m_read_iov_storage.data(), m_read_iov_count);
            case Phase::kSendPrefaceSettings:
                if (sendCompleted(m_preface_settings_buf, Phase::kRecvSettings)) {
                    continue;
                }
                return waitSend(m_preface_settings_buf);
            case Phase::kRecvSettings:
                if (tryConsumeSettingsFrame()) {
                    if (m_result.has_value()) {
                        return MachineAction<result_type>::complete(std::move(*m_result));
                    }
                    m_phase = Phase::kSendAck;
                    continue;
                }
                if (!prepareRecvWindow("RingBuffer full while waiting SETTINGS")) {
                    return MachineAction<result_type>::complete(std::move(*m_result));
                }
                return MachineAction<result_type>::waitReadv(m_read_iov_storage.data(), m_read_iov_count);
            case Phase::kSendAck:
                if (sendCompleted(m_ack_buf, Phase::kDone)) {
                    m_result = true;
                    return MachineAction<result_type>::complete(std::move(*m_result));
                }
                return waitSend(m_ack_buf);
            case Phase::kDone:
                return MachineAction<result_type>::complete(
                    m_result.value_or(std::unexpected(Http2Error(Http2ErrorCode::InternalError, "upgrade incomplete"))));
            }
        }
    }

    void onRead(std::expected<size_t, IOError> result) {
        if (!result) {
            setRecvError(result.error());
            return;
        }

        if (result.value() == 0) {
            if (m_phase == Phase::kRecvUpgradeResponse) {
                setProtocolError("peer closed while waiting 101");
            } else {
                setProtocolError("peer closed while waiting SETTINGS");
            }
            return;
        }

        m_ring_buffer->produce(result.value());
    }

    void onWrite(std::expected<size_t, IOError> result) {
        if (!result) {
            setSendError(result.error());
            return;
        }

        if (result.value() == 0) {
            setSendError(IOError(kSendFailed, 0));
            return;
        }

        const auto& payload = currentPayload();
        m_send_offset += result.value();
        if (m_send_offset > payload.size()) {
            setSendError(IOError(kSendFailed, 0));
            return;
        }
    }

private:
    enum class Phase : uint8_t {
        kSendUpgrade,
        kRecvUpgradeResponse,
        kSendPrefaceSettings,
        kRecvSettings,
        kSendAck,
        kDone,
    };

    MachineAction<result_type> waitSend(const std::string& payload) const {
        return MachineAction<result_type>::waitWrite(payload.data() + m_send_offset,
                                                     payload.size() - m_send_offset);
    }

    bool sendCompleted(const std::string& payload, Phase next_phase) {
        if (m_send_offset < payload.size()) {
            return false;
        }
        m_send_offset = 0;
        m_phase = next_phase;
        return true;
    }

    const std::string& currentPayload() const {
        switch (m_phase) {
        case Phase::kSendUpgrade:
            return m_upgrade_request_buf;
        case Phase::kSendPrefaceSettings:
            return m_preface_settings_buf;
        case Phase::kSendAck:
            return m_ack_buf;
        case Phase::kRecvUpgradeResponse:
        case Phase::kRecvSettings:
        case Phase::kDone:
            break;
        }
        return m_ack_buf;
    }

    bool prepareRecvWindow(std::string_view error_message) {
        const size_t iov_count = m_ring_buffer->getWriteIovecs(
            m_read_iov_storage.data(), m_read_iov_storage.size());
        m_read_iov_count = compactIovecs(m_read_iov_storage, iov_count);
        if (m_read_iov_count == 0) {
            setProtocolError(std::string(error_message));
            return false;
        }
        return true;
    }

    void prepareUpgradeRequest(const std::string& path) {
        Http2SettingsFrame settings_frame;
        settings_frame.addSetting(Http2SettingsId::MaxConcurrentStreams, m_client->m_config.max_concurrent_streams);
        settings_frame.addSetting(Http2SettingsId::InitialWindowSize, m_client->m_config.initial_window_size);
        std::string serialized = settings_frame.serialize();
        std::string base64_settings = galay::utils::Base64Util::Base64Encode(
            reinterpret_cast<const unsigned char*>(serialized.data() + 9), serialized.size() - 9);
        for (char& c : base64_settings) {
            if (c == '+') {
                c = '-';
            } else if (c == '/') {
                c = '_';
            }
        }
        base64_settings.erase(std::remove(base64_settings.begin(), base64_settings.end(), '='), base64_settings.end());

        auto request = Http1_1RequestBuilder::get(path)
            .host(m_client->m_authority)
            .header("Connection", "Upgrade, HTTP2-Settings")
            .header("Upgrade", "h2c")
            .header("HTTP2-Settings", base64_settings)
            .build();
        m_upgrade_request_buf = request.toString();
    }

    void preparePrefaceAndSettings() {
        std::string preface(kHttp2ConnectionPreface.begin(), kHttp2ConnectionPreface.end());
        Http2SettingsFrame settings;
        settings.addSetting(Http2SettingsId::MaxConcurrentStreams, m_client->m_config.max_concurrent_streams);
        settings.addSetting(Http2SettingsId::InitialWindowSize, m_client->m_config.initial_window_size);
        settings.header().stream_id = 0;
        m_preface_settings_buf = std::move(preface);
        m_preface_settings_buf.append(settings.serialize());
    }

    void prepareAck() {
        Http2SettingsFrame ack;
        ack.setAck(true);
        ack.header().stream_id = 0;
        m_ack_buf = ack.serialize();
    }

    void setSendError(const IOError& error) {
        setResult(Http2Error(Http2ErrorCode::InternalError, error.message()));
    }

    void setRecvError(const IOError& error) {
        if (IOError::contains(error.code(), kDisconnectError)) {
            setResult(Http2Error(Http2ErrorCode::ConnectError, "connection closed"));
            return;
        }
        setResult(Http2Error(Http2ErrorCode::InternalError, error.message()));
    }

    void setProtocolError(std::string message) {
        setResult(Http2Error(Http2ErrorCode::ProtocolError, std::move(message)));
    }

    void setConnectError(std::string message) {
        setResult(Http2Error(Http2ErrorCode::ConnectError, std::move(message)));
    }

    void setResult(Http2Error error) {
        if (!m_result.has_value()) {
            m_result = std::unexpected(std::move(error));
        }
    }

    bool parseUpgradeResponse() {
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

        if (error_code == HttpErrorCode::kHeaderInComplete || error_code == HttpErrorCode::kIncomplete) {
            return false;
        }
        if (error_code != HttpErrorCode::kNoError) {
            setProtocolError("HTTP parse error during upgrade");
            return true;
        }
        if (!m_upgrade_response.isComplete()) {
            return false;
        }
        if (m_upgrade_response.header().code() != HttpStatusCode::SwitchingProtocol_101) {
            setProtocolError("expected 101, got " +
                             std::to_string(static_cast<int>(m_upgrade_response.header().code())));
            return true;
        }
        if (!m_upgrade_response.header().headerPairs().hasKey("Upgrade")) {
            setProtocolError("missing Upgrade header");
            return true;
        }
        const std::string upgrade_value = m_upgrade_response.header().headerPairs().getValue("Upgrade");
        if (upgrade_value != "h2c") {
            setProtocolError("invalid Upgrade value: " + upgrade_value);
            return true;
        }
        return true;
    }

    bool tryConsumeSettingsFrame() {
        auto read_iovecs = borrowReadIovecs(*m_ring_buffer);
        size_t available = 0;
        for (const auto& iov : read_iovecs) {
            available += iov.iov_len;
        }
        if (available < kHttp2FrameHeaderLength) {
            return false;
        }

        uint8_t header[kHttp2FrameHeaderLength];
        size_t copied = 0;
        for (const auto& iov : read_iovecs) {
            const size_t take_count = std::min(kHttp2FrameHeaderLength - copied, iov.iov_len);
            std::memcpy(header + copied, iov.iov_base, take_count);
            copied += take_count;
            if (copied >= kHttp2FrameHeaderLength) {
                break;
            }
        }

        auto frame_header = Http2FrameHeader::deserialize(header);
        const size_t frame_size = kHttp2FrameHeaderLength + static_cast<size_t>(frame_header.length);
        if (available < frame_size) {
            return false;
        }
        if (frame_header.type != Http2FrameType::Settings) {
            setProtocolError("expected SETTINGS, got " + http2FrameTypeToString(frame_header.type));
            return true;
        }
        m_ring_buffer->consume(frame_size);
        return true;
    }

    H2cClient* m_client = nullptr;
    RingBuffer* m_ring_buffer = nullptr;
    Phase m_phase = Phase::kSendUpgrade;
    size_t m_send_offset = 0;
    std::string m_upgrade_request_buf;
    std::string m_preface_settings_buf;
    std::string m_ack_buf;
    std::array<struct iovec, 2> m_read_iov_storage{};
    size_t m_read_iov_count = 0;
    HttpResponse m_upgrade_response;
    std::optional<result_type> m_result;
};

/**
 * @brief H2c 升级 awaitable 包装器
 * @details 适配新 `StateMachineAwaitable`，并在 `await_resume()` 时完成 transport 最终接管。
 */
class H2cUpgradeAwaitable
    : public SequenceAwaitableBase
    , public TimeoutSupport<H2cUpgradeAwaitable>
{
public:
    using ResultType = std::expected<bool, Http2Error>;
    using result_type = ResultType;

    H2cUpgradeAwaitable(const H2cUpgradeAwaitable&) = delete;
    H2cUpgradeAwaitable& operator=(const H2cUpgradeAwaitable&) = delete;
    H2cUpgradeAwaitable(H2cUpgradeAwaitable&&) noexcept = default;
    H2cUpgradeAwaitable& operator=(H2cUpgradeAwaitable&&) noexcept = default;

    H2cUpgradeAwaitable(H2cClient& client, const std::string& path)
        : SequenceAwaitableBase(client.m_socket ? client.m_socket->controller() : nullptr)
        , m_client(&client)
    {
        if (client.m_socket == nullptr || client.m_ring_buffer == nullptr) {
            m_ready = true;
            m_error = Http2Error(Http2ErrorCode::ConnectError, "not connected");
            return;
        }

        m_inner_operation = std::make_unique<InnerOperation>(
            AwaitableBuilder<ResultType>::fromStateMachine(
                client.m_socket->controller(),
                H2cUpgradeMachine(client, path))
                .build()
        );
    }

    ~H2cUpgradeAwaitable() {
        cleanupInnerIfArmed();
    }

    bool await_ready() const noexcept {
        return m_ready || (m_inner_operation != nullptr && m_inner_operation->await_ready());
    }

    template <typename Promise>
    decltype(auto) await_suspend(std::coroutine_handle<Promise> handle) {
        if (m_inner_operation == nullptr) {
            return false;
        }
        m_scheduler = handle.promise().taskRefView().belongScheduler();
        m_inner_armed = true;
        return m_inner_operation->await_suspend(handle);
    }

    ResultType await_resume();

    void markTimeout() {
        m_result = std::unexpected(IOError(kTimeout, 0));
        if (m_inner_operation != nullptr) {
            m_inner_operation->markTimeout();
        }
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
    std::expected<bool, IOError> m_result{true};

private:
    using InnerOperation = galay::kernel::StateMachineAwaitable<H2cUpgradeMachine>;

    static void discardTransport(H2cClient& client);
    static bool finalizeTransport(H2cClient& client, Scheduler* scheduler);
    static Http2Error translateIoError(const IOError& error);

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

    H2cClient* m_client = nullptr;
    std::unique_ptr<InnerOperation> m_inner_operation;
    std::optional<Http2Error> m_error;
    Scheduler* m_scheduler = nullptr;
    bool m_ready = false;
    bool m_inner_armed = false;
    bool m_inner_completed = false;
};

inline H2cClient H2cClientBuilder::build() const { return H2cClient(m_config); }

inline void H2cUpgradeAwaitable::discardTransport(H2cClient& client) {
    if (client.m_conn != nullptr) {
        if (client.m_conn->socket().handle().fd >= 0) {
            ::close(client.m_conn->socket().handle().fd);
        }
        client.m_conn.reset();
    }
    if (client.m_socket != nullptr && client.m_socket->handle().fd >= 0) {
        ::close(client.m_socket->handle().fd);
    }
    client.m_socket.reset();
    client.m_ring_buffer.reset();
    client.m_upgraded = false;
}

inline bool H2cUpgradeAwaitable::finalizeTransport(H2cClient& client, Scheduler* scheduler) {
    if (scheduler == nullptr || client.m_socket == nullptr || client.m_ring_buffer == nullptr) {
        return false;
    }

    client.m_conn = std::make_unique<Http2ConnImpl<TcpSocket>>(
        std::move(*client.m_socket), std::move(*client.m_ring_buffer));
    client.m_conn->localSettings().from(client.m_config);
    client.m_conn->runtimeConfig().from(client.m_config);
    client.m_conn->markSettingsSent();
    client.m_conn->setIsClient(true);
    client.m_conn->initStreamManager();

    auto* manager = client.m_conn->streamManager();
    if (manager == nullptr) {
        return false;
    }

    if (!manager->startWithScheduler(
            scheduler,
            [](Http2Stream::ptr) -> Task<void> { co_return; })) {
        client.m_upgrade_result = false;
        return false;
    }

    client.m_socket.reset();
    client.m_ring_buffer.reset();
    client.m_upgraded = true;
    client.m_upgrade_result = true;
    return true;
}

inline Http2Error H2cUpgradeAwaitable::translateIoError(const IOError& error) {
    if (IOError::contains(error.code(), kTimeout)) {
        return Http2Error(Http2ErrorCode::ConnectError, "upgrade timeout");
    }
    if (IOError::contains(error.code(), kDisconnectError)) {
        return Http2Error(Http2ErrorCode::ConnectError, error.message());
    }
    return Http2Error(Http2ErrorCode::InternalError, error.message());
}

inline std::expected<bool, Http2Error> H2cUpgradeAwaitable::await_resume() {
    const auto fail = [this](Http2Error error) -> ResultType {
        discardTransport(*m_client);
        m_client->m_upgrade_result = std::unexpected(error);
        return std::unexpected(std::move(error));
    };

    if (!m_result.has_value()) {
        cleanupInnerIfArmed();
        return fail(translateIoError(m_result.error()));
    }

    if (m_error.has_value()) {
        cleanupInnerIfArmed();
        return fail(*m_error);
    }

    if (m_ready) {
        return fail(Http2Error(Http2ErrorCode::ConnectError, "not connected"));
    }

    if (m_inner_operation != nullptr && m_inner_operation->m_error.has_value()) {
        cleanupInnerIfArmed();
        return fail(translateIoError(*m_inner_operation->m_error));
    }

    auto result = resumeInner();
    if (!result) {
        return fail(result.error());
    }

    if (!finalizeTransport(*m_client, m_scheduler)) {
        return fail(Http2Error(Http2ErrorCode::InternalError, "failed to finalize h2c transport"));
    }

    return result;
}

inline H2cUpgradeAwaitable H2cClient::upgrade(const std::string& path) {
    return H2cUpgradeAwaitable(*this, path);
}

// ============== get() / post() ==============

inline Http2Stream::ptr H2cClient::get(const std::string& path) {
    if (!m_conn || !m_conn->streamManager()) {
        return nullptr;
    }
    auto* mgr = m_conn->streamManager();
    auto stream = mgr->allocateStream();
    std::vector<Http2HeaderField> headers;
    headers.reserve(4);
    headers.push_back({":method", "GET"});
    headers.push_back({":scheme", "http"});
    headers.push_back({":authority", m_authority});
    headers.push_back({":path", path.empty() ? "/" : path});
    stream->sendHeaders(headers, true);
    return stream;
}

inline Http2Stream::ptr H2cClient::post(const std::string& path,
                                         const std::string& body,
                                         const std::string& content_type) {
    if (!m_conn || !m_conn->streamManager()) {
        return nullptr;
    }
    auto* mgr = m_conn->streamManager();
    auto stream = mgr->allocateStream();
    std::vector<Http2HeaderField> headers;
    headers.reserve(5);
    headers.push_back({":method", "POST"});
    headers.push_back({":scheme", "http"});
    headers.push_back({":authority", m_authority});
    headers.push_back({":path", path.empty() ? "/" : path});
    headers.push_back({"content-type", content_type});
    stream->sendHeaders(headers, false);
    stream->sendData(body, true);
    return stream;
}

inline Task<std::expected<bool, Http2Error>> H2cClient::shutdown() {
    m_shutdown_result = true;


    if (m_conn && m_conn->streamManager()) {
        co_await m_conn->streamManager()->shutdown(Http2ErrorCode::NoError);
    } else if (m_socket) {
        auto close_result = co_await m_socket->close();
        if (!close_result) {
            m_shutdown_result = std::unexpected(
                Http2Error(Http2ErrorCode::InternalError, close_result.error().message()));
        }
    }

    m_conn.reset();
    m_socket.reset();
    m_ring_buffer.reset();
    m_upgraded = false;

    if (!m_shutdown_result) {
        co_return m_shutdown_result;
    }

    m_shutdown_result = true;
    co_return m_shutdown_result;
}

} // namespace galay::http2

#endif // GALAY_H2C_CLIENT_H
