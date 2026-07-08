/**
 * @file ws_session.h
 * @brief WebSocket 会话，整合连接、升级和通信功能
 * @author galay-http
 * @version 1.0.0
 *
 * @details 提供 WsSessionImpl 模板类，整合 WebSocket 连接的升级握手、
 *          消息读写和便捷方法。内部使用状态机驱动 TCP 和 SSL 两种升级流程。
 */

#ifndef GALAY_WS_SESSION_H
#define GALAY_WS_SESSION_H

#include "../kernel/ws_reader.h"
#include "../kernel/ws_writer.h"
#include "../server/ws_upgrade.h"
#include "ws_url.h"
#include "../../galay-http/protoc/http_response.h"
#include "../../galay-http/builder/http_builder.h"
#include "../../galay-utils/cache/bytes.hpp"
#include "../../galay-utils/cache/ring_buffer.hpp"
#include "../../galay-kernel/async/tcp_socket.h"
#include "../../galay-kernel/core/awaitable.h"
#include "../../galay-kernel/core/io_handlers.hpp"
#include "../../galay-utils/encoding/base64.hpp"
#include <memory>
#include <string>
#include <optional>
#include <coroutine>
#include <utility>
#include <vector>

#ifdef GALAY_SSL_FEATURE_ENABLED
#include "../../galay-ssl/async/ssl_await.h"
#include "../../galay-ssl/async/ssl_socket.h"
#endif

namespace galay::websocket
{

using namespace galay::async;
using namespace galay::kernel;
using ::galay::utils::Bytes;
using ::galay::utils::RingBuffer;
using namespace galay::http;

template<typename SocketType>
class WsSessionImpl;

template<typename SocketType>
class WsSessionUpgraderImpl;

namespace detail {
template<typename SocketType, bool IsSsl = is_ssl_socket_v<SocketType>>
class WsSessionUpgradeOperation;
}

/**
 * @brief WebSocket Session 升级器
 * @details 管理升级过程中的临时变量和状态
 */
template<typename SocketType>
class WsSessionUpgraderImpl
{
public:
    WsSessionUpgraderImpl(WsSessionImpl<SocketType>* session)
        : m_session(session)
    {
    }

    /**
     * @brief 返回升级 operation
     * @return 可以 co_await 的 operation 对象
     */
    auto operator()() {
        return detail::WsSessionUpgradeOperation<SocketType>(this);
    }

    friend class detail::WsSessionUpgradeOperation<SocketType, false>;
#ifdef GALAY_SSL_FEATURE_ENABLED
    friend class detail::WsSessionUpgradeOperation<SocketType, true>;
#endif

private:
    WsSessionImpl<SocketType>* m_session;
};

namespace detail {

/**
 * @brief WebSocket Session 升级 operation - TcpSocket 版本（AwaitableBuilder SEND+RECV+PARSE）
 */
template<typename SocketType>
class WsSessionUpgradeOperation<SocketType, false>
    : public SequenceAwaitableBase
    , public galay::kernel::TimeoutSupport<WsSessionUpgradeOperation<SocketType, false>>
{
public:
    using ResultType = std::expected<bool, WsError>;
    using result_type = ResultType;

    WsSessionUpgradeOperation(const WsSessionUpgradeOperation&) = delete;
    WsSessionUpgradeOperation& operator=(const WsSessionUpgradeOperation&) = delete;
    WsSessionUpgradeOperation(WsSessionUpgradeOperation&&) noexcept = default;
    WsSessionUpgradeOperation& operator=(WsSessionUpgradeOperation&&) noexcept = default;

    struct UpgradeFlow {
        explicit UpgradeFlow(WsSessionUpgraderImpl<SocketType>* upgrader)
            : m_upgrader(upgrader)
            , m_recv_buffer(std::max<size_t>(upgrader->m_session->m_ring_buffer.capacity(), 1))
        {
            initUpgradeRequest();
        }

        void onSend(SequenceOps<ResultType, 4>& ops, SendIOContext& send_ctx) {
            if (!send_ctx.m_result) {
                ops.complete(std::unexpected(WsError(kWsSendError, send_ctx.m_result.error().message())));
            }
        }

        void onRecv(SequenceOps<ResultType, 4>& ops, RecvIOContext& recv_ctx) {
            if (!recv_ctx.m_result) {
                const auto& error = recv_ctx.m_result.error();
                if (IOError::contains(error.code(), kDisconnectError)) {
                    ops.complete(std::unexpected(WsError(kWsConnectionClosed, error.message())));
                    return;
                }
                ops.complete(std::unexpected(WsError(kWsConnectionError, error.message())));
                return;
            }

            const size_t recv_bytes = recv_ctx.m_result.value();
            if (recv_bytes == 0) {
                ops.complete(std::unexpected(WsError(kWsConnectionClosed, "Connection closed")));
                return;
            }

            auto& session = *m_upgrader->m_session;
            if (session.m_ring_buffer.write(m_recv_buffer.data(), recv_bytes) != recv_bytes) {
                ops.complete(std::unexpected(WsError(kWsProtocolError, "Upgrade response too large")));
            }
        }

        ParseStatus onParse(SequenceOps<ResultType, 4>& ops) {
            auto& session = *m_upgrader->m_session;
            auto iovecs = borrowReadIovecs(session.m_ring_buffer);
            if (iovecs.empty()) {
                return ParseStatus::kNeedMore;
            }

            std::vector<iovec> parse_iovecs;
            if (IoVecWindow::buildWindow(iovecs, parse_iovecs) == 0) {
                return ParseStatus::kNeedMore;
            }

            auto [error_code, consumed] = m_upgrade_response.fromIOVec(parse_iovecs);
            if (consumed > 0) {
                session.m_ring_buffer.consume(consumed);
            }

            if (error_code == kIncomplete || error_code == kHeaderInComplete) {
                if (session.m_ring_buffer.full()) {
                    ops.complete(std::unexpected(WsError(kWsProtocolError, "Upgrade response too large")));
                    return ParseStatus::kCompleted;
                }
                return ParseStatus::kNeedMore;
            }

            if (error_code != kNoError) {
                ops.complete(std::unexpected(WsError(kWsProtocolError, "Failed to parse upgrade response")));
                return ParseStatus::kCompleted;
            }

            if (!m_upgrade_response.isComplete()) {
                if (session.m_ring_buffer.full()) {
                    ops.complete(std::unexpected(WsError(kWsProtocolError, "Upgrade response too large")));
                    return ParseStatus::kCompleted;
                }
                return ParseStatus::kNeedMore;
            }

            if (m_upgrade_response.header().code() != HttpStatusCode::SwitchingProtocol_101) {
                ops.complete(std::unexpected(WsError(
                    kWsUpgradeFailed,
                    "Upgrade failed with status " +
                        std::to_string(static_cast<int>(m_upgrade_response.header().code()))
                )));
                return ParseStatus::kCompleted;
            }

            if (!m_upgrade_response.header().headerPairs().hasKey("Sec-WebSocket-Accept")) {
                ops.complete(std::unexpected(WsError(kWsUpgradeFailed, "Missing Sec-WebSocket-Accept header")));
                return ParseStatus::kCompleted;
            }

            std::string accept_key = m_upgrade_response.header().headerPairs().getValue("Sec-WebSocket-Accept");
            std::string expected_accept = WsUpgrade::generateAcceptKey(m_ws_key);
            if (accept_key != expected_accept) {
                ops.complete(std::unexpected(WsError(kWsUpgradeFailed, "Invalid Sec-WebSocket-Accept value")));
                return ParseStatus::kCompleted;
            }

            session.m_upgraded = true;
            ops.complete(true);
            return ParseStatus::kCompleted;
        }

        void initUpgradeRequest() {
            auto& session = *m_upgrader->m_session;
            m_ws_key = generateWebSocketKey();

            auto request = Http1_1RequestBuilder::get(session.m_url.path)
                .header("Host", session.m_url.host + ":" + std::to_string(session.m_url.port))
                .header("Upgrade", "websocket")
                .header("Connection", "Upgrade")
                .header("Sec-WebSocket-Key", m_ws_key)
                .header("Sec-WebSocket-Version", "13")
                .build();

            m_send_buffer = request.toString();
        }

        WsSessionUpgraderImpl<SocketType>* m_upgrader;
        std::string m_ws_key;
        std::string m_send_buffer;
        HttpResponse m_upgrade_response;
        std::vector<char> m_recv_buffer;
    };

private:
    using InnerMachine = galay::kernel::detail::LinearMachine<ResultType, 4, UpgradeFlow>;
    using InnerOperation = galay::kernel::StateMachineAwaitable<InnerMachine>;

public:
    explicit WsSessionUpgradeOperation(WsSessionUpgraderImpl<SocketType>* upgrader)
        : SequenceAwaitableBase(upgrader->m_session->m_socket.controller())
        , m_result(true)
    {
        if (upgrader->m_session->m_upgraded) {
            m_ready = true;
            return;
        }

        m_flow = std::make_unique<UpgradeFlow>(upgrader);
        m_inner_operation = std::make_unique<InnerOperation>(
            AwaitableBuilder<ResultType, 4, UpgradeFlow>(upgrader->m_session->m_socket.controller(), *m_flow)
                .template send<&UpgradeFlow::onSend>(m_flow->m_send_buffer.data(), m_flow->m_send_buffer.size())
                .template recv<&UpgradeFlow::onRecv>(m_flow->m_recv_buffer.data(), m_flow->m_recv_buffer.size())
                .template parse<&UpgradeFlow::onParse>()
                .build()
        );
    }

    ~WsSessionUpgradeOperation() {
        cleanupInnerIfArmed();
    }

    bool await_ready() noexcept {
        return m_ready || (m_inner_operation != nullptr && m_inner_operation->await_ready());
    }

    template<typename Promise>
    decltype(auto) await_suspend(std::coroutine_handle<Promise> handle) {
        m_inner_armed = true;
        return m_inner_operation->await_suspend(handle);
    }

    ResultType await_resume() {
        if (!m_result.has_value()) {
            cleanupInnerIfArmed();
            const auto& io_error = m_result.error();
            if (IOError::contains(io_error.code(), kTimeout)) {
                return std::unexpected(WsError(kWsConnectionError, "Upgrade timeout"));
            }
            if (IOError::contains(io_error.code(), kDisconnectError)) {
                return std::unexpected(WsError(kWsConnectionClosed, io_error.message()));
            }
            return std::unexpected(WsError(kWsConnectionError, io_error.message()));
        }

        if (m_ready) {
            return true;
        }
        return resumeInner();
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

    std::expected<bool, galay::kernel::IOError> m_result;

private:
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

    std::unique_ptr<UpgradeFlow> m_flow;
    std::unique_ptr<InnerOperation> m_inner_operation;
    bool m_ready = false;
    bool m_inner_armed = false;
    bool m_inner_completed = false;
};

#ifdef GALAY_SSL_FEATURE_ENABLED
/**
 * @brief WebSocket Session 升级 operation - SslSocket 版本（SSL 状态机）
 */
template<typename SocketType>
class WsSessionUpgradeOperation<SocketType, true>
    : public SequenceAwaitableBase
    , public galay::kernel::TimeoutSupport<WsSessionUpgradeOperation<SocketType, true>>
{
public:
    using ResultType = std::expected<bool, WsError>;
    using result_type = ResultType;

    WsSessionUpgradeOperation(const WsSessionUpgradeOperation&) = delete;
    WsSessionUpgradeOperation& operator=(const WsSessionUpgradeOperation&) = delete;
    WsSessionUpgradeOperation(WsSessionUpgradeOperation&&) noexcept = default;
    WsSessionUpgradeOperation& operator=(WsSessionUpgradeOperation&&) noexcept = default;

    struct UpgradeState {
        explicit UpgradeState(WsSessionUpgraderImpl<SocketType>* upgrader)
            : m_upgrader(upgrader)
            , m_recv_buffer(std::max<size_t>(upgrader->m_session->m_ring_buffer.capacity(), 1))
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

        void onBytesSent(size_t sent_bytes) {
            if (sent_bytes == 0) {
                setProtocolError("Connection closed");
                return;
            }
            if (sent_bytes > remainingSendBytes()) {
                setProtocolError("Send progress overflow");
                return;
            }
            m_send_offset += sent_bytes;
        }

        bool prepareRecvWindow(char*& buffer, size_t& length) {
            auto& session = *m_upgrader->m_session;
            if (session.m_ring_buffer.full()) {
                buffer = nullptr;
                length = 0;
                return false;
            }

            buffer = m_recv_buffer.data();
            length = m_recv_buffer.size();
            return length > 0;
        }

        void onBytesReceived(size_t recv_bytes) {
            auto& session = *m_upgrader->m_session;
            if (session.m_ring_buffer.write(m_recv_buffer.data(), recv_bytes) != recv_bytes) {
                setProtocolError("Upgrade response too large");
            }
        }

        bool tryParseUpgradeResponse() {
            auto& session = *m_upgrader->m_session;
            auto iovecs = borrowReadIovecs(session.m_ring_buffer);
            if (iovecs.empty()) {
                return false;
            }

            std::vector<iovec> parse_iovecs;
            if (IoVecWindow::buildWindow(iovecs, parse_iovecs) == 0) {
                return false;
            }

            auto [error_code, consumed] = m_upgrade_response.fromIOVec(parse_iovecs);
            if (consumed > 0) {
                session.m_ring_buffer.consume(consumed);
            }

            if (error_code == kIncomplete || error_code == kHeaderInComplete) {
                if (session.m_ring_buffer.full()) {
                    setProtocolError("Upgrade response too large");
                    return true;
                }
                return false;
            }

            if (error_code != kNoError) {
                setProtocolError("Failed to parse upgrade response");
                return true;
            }

            if (!m_upgrade_response.isComplete()) {
                return false;
            }

            if (m_upgrade_response.header().code() != HttpStatusCode::SwitchingProtocol_101) {
                m_error = WsError(
                    kWsUpgradeFailed,
                    "Upgrade failed with status " +
                        std::to_string(static_cast<int>(m_upgrade_response.header().code())));
                return true;
            }

            if (!m_upgrade_response.header().headerPairs().hasKey("Sec-WebSocket-Accept")) {
                m_error = WsError(kWsUpgradeFailed, "Missing Sec-WebSocket-Accept header");
                return true;
            }

            std::string accept_key = m_upgrade_response.header().headerPairs().getValue("Sec-WebSocket-Accept");
            if (accept_key != WsUpgrade::generateAcceptKey(m_ws_key)) {
                m_error = WsError(kWsUpgradeFailed, "Invalid Sec-WebSocket-Accept value");
                return true;
            }

            session.m_upgraded = true;
            m_result = true;
            return true;
        }

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

        void setProtocolError(std::string message) {
            m_error = WsError(kWsProtocolError, std::move(message));
        }

    private:
        void initialize() {
            auto& session = *m_upgrader->m_session;
            if (session.m_upgraded) {
                m_result = true;
                return;
            }

            m_ws_key = generateWebSocketKey();

            auto request = Http1_1RequestBuilder::get(session.m_url.path)
                .header("Host", session.m_url.host + ":" + std::to_string(session.m_url.port))
                .header("Upgrade", "websocket")
                .header("Connection", "Upgrade")
                .header("Sec-WebSocket-Key", m_ws_key)
                .header("Sec-WebSocket-Version", "13")
                .build();

            m_send_buffer = request.toString();
        }

        WsSessionUpgraderImpl<SocketType>* m_upgrader;
        std::string m_ws_key;
        std::string m_send_buffer;
        size_t m_send_offset = 0;
        HttpResponse m_upgrade_response;
        std::vector<char> m_recv_buffer;
        std::optional<ResultType> m_result;
        std::optional<WsError> m_error;
    };

    struct UpgradeMachine {
        using result_type = ResultType;

        explicit UpgradeMachine(std::shared_ptr<UpgradeState> state)
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

        std::shared_ptr<UpgradeState> m_state;
    };

private:
    using InnerOperation = galay::ssl::SslStateMachineAwaitable<UpgradeMachine>;

public:
    explicit WsSessionUpgradeOperation(WsSessionUpgraderImpl<SocketType>* upgrader)
        : SequenceAwaitableBase(upgrader->m_session->m_socket.controller())
        , m_result(true)
    {
        if (upgrader->m_session->m_upgraded) {
            m_ready = true;
            return;
        }

        auto state = std::make_shared<UpgradeState>(upgrader);
        m_inner_operation = std::make_unique<InnerOperation>(
            galay::ssl::SslAwaitableBuilder<ResultType>::fromStateMachine(
                upgrader->m_session->m_socket.controller(),
                &upgrader->m_session->m_socket,
                UpgradeMachine(std::move(state)))
                .build()
        );
    }

    ~WsSessionUpgradeOperation() {
        cleanupInnerIfArmed();
    }

    bool await_ready() noexcept {
        return m_ready || (m_inner_operation != nullptr && m_inner_operation->await_ready());
    }

    template<typename Promise>
    decltype(auto) await_suspend(std::coroutine_handle<Promise> handle) {
        if (m_inner_operation == nullptr) {
            return false;
        }
        m_inner_armed = true;
        return m_inner_operation->await_suspend(handle);
    }

    ResultType await_resume() {
        if (!m_result.has_value()) {
            cleanupInnerIfArmed();
            const auto& io_error = m_result.error();
            if (IOError::contains(io_error.code(), kTimeout)) {
                return std::unexpected(WsError(kWsConnectionError, "Upgrade timeout"));
            }
            if (IOError::contains(io_error.code(), kDisconnectError)) {
                return std::unexpected(WsError(kWsConnectionClosed, io_error.message()));
            }
            return std::unexpected(WsError(kWsConnectionError, io_error.message()));
        }

        if (m_ready) {
            return true;
        }
        return resumeInner();
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
    std::expected<bool, galay::kernel::IOError> m_result;

private:
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

    std::unique_ptr<InnerOperation> m_inner_operation;
    bool m_ready = false;
    bool m_inner_armed = false;
    bool m_inner_completed = false;
};
#endif

} // namespace detail

/**
 * @brief WebSocket会话模板类
 * @details 持有 socket、ring_buffer、reader 和 writer，负责WebSocket升级和通信
 */
template<typename SocketType>
class WsSessionImpl
{
public:
    WsSessionImpl(SocketType& socket,
                  const WsUrl& url,
                  const WsWriterSetting& writer_setting,
                  size_t ring_buffer_size = 8192,
                  const WsReaderSetting& reader_setting = WsReaderSetting())
        : m_socket(socket)
        , m_url(url)
        , m_ring_buffer(ring_buffer_size)
        , m_reader(m_ring_buffer, reader_setting, socket, false, true)  // is_server=false, use_mask=true (客户端)
        , m_writer(writer_setting, socket)
        , m_upgraded(false)
    {
    }

    WsReaderImpl<SocketType>& getReader() {
        return m_reader;
    }

    WsWriterImpl<SocketType>& getWriter() {
        return m_writer;
    }

    /**
     * @brief 执行WebSocket升级握手
     * @return 升级器对象
     */
    WsSessionUpgraderImpl<SocketType> upgrade() {
        return WsSessionUpgraderImpl<SocketType>(this);
    }

    bool isUpgraded() const {
        return m_upgraded;
    }

    // 便捷方法：发送文本消息
    auto sendText(const std::string& text, bool fin = true) {
        return m_writer.sendText(text, fin);
    }

    // 便捷方法：发送文本消息（移动语义）
    auto sendText(std::string&& text, bool fin = true) {
        return m_writer.sendText(std::move(text), fin);
    }

    // 便捷方法：发送二进制消息
    auto sendBinary(const std::string& data, bool fin = true) {
        return m_writer.sendBinary(data, fin);
    }

    // 便捷方法：发送二进制消息（移动语义）
    auto sendBinary(std::string&& data, bool fin = true) {
        return m_writer.sendBinary(std::move(data), fin);
    }

    // 便捷方法：发送Ping
    auto sendPing(const std::string& data = "") {
        return m_writer.sendPing(data);
    }

    // 便捷方法：发送Pong
    auto sendPong(const std::string& data = "") {
        return m_writer.sendPong(data);
    }

    // 便捷方法：发送Close
    auto sendClose(WsCloseCode code = WsCloseCode::Normal, const std::string& reason = "") {
        return m_writer.sendClose(code, reason);
    }

    // 便捷方法：接收消息
    auto getMessage(std::string& message, WsOpcode& opcode) {
        return m_reader.getMessage(message, opcode);
    }

    // 便捷方法：接收帧
    auto getFrame(WsFrame& frame) {
        return m_reader.getFrame(frame);
    }

    friend class detail::WsSessionUpgradeOperation<SocketType, false>;
#ifdef GALAY_SSL_FEATURE_ENABLED
    friend class detail::WsSessionUpgradeOperation<SocketType, true>;
#endif
    friend class WsSessionUpgraderImpl<SocketType>;

private:
    SocketType& m_socket;
    const WsUrl& m_url;
    RingBuffer<> m_ring_buffer;
    WsReaderImpl<SocketType> m_reader;
    WsWriterImpl<SocketType> m_writer;
    bool m_upgraded;
};

using WsSession = WsSessionImpl<TcpSocket>;

} // namespace galay::websocket

#ifdef GALAY_SSL_FEATURE_ENABLED
#include "../../galay-ssl/async/ssl_socket.h"

namespace galay::websocket {

using WssSession = WsSessionImpl<galay::ssl::SslSocket>;

} // namespace galay::websocket
#endif

#endif // GALAY_WS_SESSION_H
