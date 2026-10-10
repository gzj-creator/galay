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
#include "../../galay-utils/buffer/bytes.hpp"
#include "../../galay-utils/buffer/ring_buffer.hpp"
#include "../../galay-kernel/async/async_tcp.h"
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

private:
    WsSessionUpgraderImpl(const WsSessionUpgraderImpl&) = delete;
    WsSessionUpgraderImpl& operator=(const WsSessionUpgraderImpl&) = delete;
public:
    WsSessionUpgraderImpl(WsSessionUpgraderImpl&&) noexcept = default;
    WsSessionUpgraderImpl& operator=(WsSessionUpgraderImpl&&) noexcept = default;

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
 * @brief WebSocket Session 升级 operation - AsyncTcpSocket 版本（AwaitableBuilder SEND+RECV+PARSE）
 */
template<typename SocketType>
class WsSessionUpgradeOperation<SocketType, false>
    : public SequenceAwaitableBase
    , public galay::kernel::TimeoutMethods<WsSessionUpgradeOperation<SocketType, false>>
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
            init_upgrade_request();
        }

    private:
        UpgradeFlow(const UpgradeFlow&) = delete;
        UpgradeFlow& operator=(const UpgradeFlow&) = delete;
    public:
        UpgradeFlow(UpgradeFlow&&) noexcept = default;
        UpgradeFlow& operator=(UpgradeFlow&&) noexcept = default;

        void on_send(SequenceOps<ResultType, 4>& ops, SendIOContext& send_ctx) {
            if (!send_ctx.m_result) {
                ops.complete(std::unexpected(WsError(kWsSendError, send_ctx.m_result.error().message())));
            }
        }

        void on_recv(SequenceOps<ResultType, 4>& ops, RecvIOContext& recv_ctx) {
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
            if (session.m_ring_buffer.try_write_batch(
                    m_recv_buffer.data(), recv_bytes) != recv_bytes) {
                ops.complete(std::unexpected(WsError(kWsProtocolError, "Upgrade response too large")));
            }
        }

        ParseStatus on_parse(SequenceOps<ResultType, 4>& ops) {
            auto& session = *m_upgrader->m_session;
            auto iovecs = borrow_read_iovecs(session.m_ring_buffer);
            if (iovecs.empty()) {
                return ParseStatus::kNeedMore;
            }

            std::vector<iovec> parse_iovecs;
            if (IoVecWindow::build_window(iovecs, parse_iovecs) == 0) {
                return ParseStatus::kNeedMore;
            }

            auto [error_code, consumed] = m_upgrade_response.from_io_vec(parse_iovecs);
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

            if (!m_upgrade_response.is_complete()) {
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

            if (!m_upgrade_response.header().header_pairs().has_key("Sec-WebSocket-Accept")) {
                ops.complete(std::unexpected(WsError(kWsUpgradeFailed, "Missing Sec-WebSocket-Accept header")));
                return ParseStatus::kCompleted;
            }

            std::string accept_key = m_upgrade_response.header().header_pairs().get_value("Sec-WebSocket-Accept");
            std::string expected_accept = WsUpgrade::generate_accept_key(m_ws_key);
            if (accept_key != expected_accept) {
                ops.complete(std::unexpected(WsError(kWsUpgradeFailed, "Invalid Sec-WebSocket-Accept value")));
                return ParseStatus::kCompleted;
            }

            session.m_upgraded = true;
            ops.complete(true);
            return ParseStatus::kCompleted;
        }

        void init_upgrade_request() {
            auto& session = *m_upgrader->m_session;
            m_ws_key = generate_web_socket_key();

            auto request = Http1_1RequestBuilder::get(session.m_url.path)
                .header("Host", session.m_url.host + ":" + std::to_string(session.m_url.port))
                .header("Upgrade", "websocket")
                .header("Connection", "Upgrade")
                .header("Sec-WebSocket-Key", m_ws_key)
                .header("Sec-WebSocket-Version", "13")
                .build();

            m_send_buffer = request.to_string();
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
                .template send<&UpgradeFlow::on_send>(m_flow->m_send_buffer.data(), m_flow->m_send_buffer.size())
                .template recv<&UpgradeFlow::on_recv>(m_flow->m_recv_buffer.data(), m_flow->m_recv_buffer.size())
                .template parse<&UpgradeFlow::on_parse>()
                .build()
        );
    }

    ~WsSessionUpgradeOperation() {
        cleanup_inner_if_armed();
    }

    bool await_ready() noexcept {
        return m_ready || (m_inner_operation != nullptr && m_inner_operation->await_ready());
    }

    template<typename Promise>
    decltype(auto) await_suspend(std::coroutine_handle<Promise> handle) {
        if (m_inner_operation == nullptr) {
            cancel_bound_timeout_timer();
            return false;
        }
        forward_bound_timeout_timer(*m_inner_operation);
        m_inner_armed = true;
        return m_inner_operation->await_suspend(handle);
    }

    /**
     * @brief 暂存外层 timeout 绑定，并在 await_suspend() 中转交给 inner。
     * @param timer 定时器
     * @return 无返回值
     */
    void bind_timeout_timer(TimeoutTimer* timer) noexcept {
        SequenceAwaitableBase::bind_timeout_timer(timer);
    }

    ResultType await_resume() {
        if (!m_result.has_value()) {
            cleanup_inner_if_armed();
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
        return resume_inner();
    }

    IOTask* front() override {
        return m_inner_operation ? m_inner_operation->front() : nullptr;
    }

    const IOTask* front() const override {
        return m_inner_operation ? m_inner_operation->front() : nullptr;
    }

    void pop_front() override {
        if (m_inner_operation) {
            m_inner_operation->pop_front();
        }
    }

    bool empty() const override {
        return m_inner_operation == nullptr || m_inner_operation->empty();
    }

#ifdef USE_IOURING
    SequenceProgress prepare_for_submit() override {
        return m_inner_operation ? m_inner_operation->prepare_for_submit() : SequenceProgress::kCompleted;
    }

    SequenceProgress on_active_event(struct io_uring_cqe* cqe, GHandle handle) override {
        return m_inner_operation ? m_inner_operation->on_active_event(cqe, handle) : SequenceProgress::kCompleted;
    }
#else
    SequenceProgress prepare_for_submit(GHandle handle) override {
        return m_inner_operation ? m_inner_operation->prepare_for_submit(handle) : SequenceProgress::kCompleted;
    }

    SequenceProgress on_active_event(GHandle handle) override {
        return m_inner_operation ? m_inner_operation->on_active_event(handle) : SequenceProgress::kCompleted;
    }
#endif

    std::expected<bool, galay::kernel::IOError> m_result;

private:
    ResultType resume_inner() {
        m_inner_completed = true;
        return m_inner_operation->await_resume();
    }

    void cleanup_inner_if_armed() {
        if (m_inner_operation != nullptr && m_inner_armed && !m_inner_completed) {
            m_inner_operation->on_completed();
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
    , public galay::kernel::TimeoutMethods<WsSessionUpgradeOperation<SocketType, true>>
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

    private:
        UpgradeState(const UpgradeState&) = delete;
        UpgradeState& operator=(const UpgradeState&) = delete;
    public:
        UpgradeState(UpgradeState&&) noexcept = default;
        UpgradeState& operator=(UpgradeState&&) noexcept = default;

        bool is_finished() const {
            return m_result.has_value() || m_error.has_value();
        }

        ResultType take_result() {
            if (m_error.has_value()) {
                return std::unexpected(std::move(*m_error));
            }
            return m_result.value_or(ResultType(true));
        }

        bool has_pending_send() const {
            return !is_finished() && m_send_offset < m_send_buffer.size();
        }

        const char* send_data() const {
            return m_send_buffer.data() + m_send_offset;
        }

        size_t remaining_send_bytes() const {
            return m_send_buffer.size() - m_send_offset;
        }

        void on_bytes_sent(size_t sent_bytes) {
            if (sent_bytes == 0) {
                set_protocol_error("Connection closed");
                return;
            }
            if (sent_bytes > remaining_send_bytes()) {
                set_protocol_error("Send progress overflow");
                return;
            }
            m_send_offset += sent_bytes;
        }

        bool prepare_recv_window(char*& buffer, size_t& length) {
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

        void on_bytes_received(size_t recv_bytes) {
            auto& session = *m_upgrader->m_session;
            if (session.m_ring_buffer.try_write_batch(
                    m_recv_buffer.data(), recv_bytes) != recv_bytes) {
                set_protocol_error("Upgrade response too large");
            }
        }

        bool try_parse_upgrade_response() {
            auto& session = *m_upgrader->m_session;
            auto iovecs = borrow_read_iovecs(session.m_ring_buffer);
            if (iovecs.empty()) {
                return false;
            }

            std::vector<iovec> parse_iovecs;
            if (IoVecWindow::build_window(iovecs, parse_iovecs) == 0) {
                return false;
            }

            auto [error_code, consumed] = m_upgrade_response.from_io_vec(parse_iovecs);
            if (consumed > 0) {
                session.m_ring_buffer.consume(consumed);
            }

            if (error_code == kIncomplete || error_code == kHeaderInComplete) {
                if (session.m_ring_buffer.full()) {
                    set_protocol_error("Upgrade response too large");
                    return true;
                }
                return false;
            }

            if (error_code != kNoError) {
                set_protocol_error("Failed to parse upgrade response");
                return true;
            }

            if (!m_upgrade_response.is_complete()) {
                return false;
            }

            if (m_upgrade_response.header().code() != HttpStatusCode::SwitchingProtocol_101) {
                m_error = WsError(
                    kWsUpgradeFailed,
                    "Upgrade failed with status " +
                        std::to_string(static_cast<int>(m_upgrade_response.header().code())));
                return true;
            }

            if (!m_upgrade_response.header().header_pairs().has_key("Sec-WebSocket-Accept")) {
                m_error = WsError(kWsUpgradeFailed, "Missing Sec-WebSocket-Accept header");
                return true;
            }

            std::string accept_key = m_upgrade_response.header().header_pairs().get_value("Sec-WebSocket-Accept");
            if (accept_key != WsUpgrade::generate_accept_key(m_ws_key)) {
                m_error = WsError(kWsUpgradeFailed, "Invalid Sec-WebSocket-Accept value");
                return true;
            }

            session.m_upgraded = true;
            m_result = true;
            return true;
        }

        void set_ssl_send_error(const galay::ssl::SslError& error) {
            m_error = WsError(error);
        }

        void set_ssl_recv_error(const galay::ssl::SslError& error) {
            m_error = WsError(error);
        }

        void set_protocol_error(std::string message) {
            m_error = WsError(kWsProtocolError, std::move(message));
        }

    private:
        void initialize() {
            auto& session = *m_upgrader->m_session;
            if (session.m_upgraded) {
                m_result = true;
                return;
            }

            m_ws_key = generate_web_socket_key();

            auto request = Http1_1RequestBuilder::get(session.m_url.path)
                .header("Host", session.m_url.host + ":" + std::to_string(session.m_url.port))
                .header("Upgrade", "websocket")
                .header("Connection", "Upgrade")
                .header("Sec-WebSocket-Key", m_ws_key)
                .header("Sec-WebSocket-Version", "13")
                .build();

            m_send_buffer = request.to_string();
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
            if (m_state->is_finished()) {
                return galay::ssl::SslMachineAction<result_type>::complete(m_state->take_result());
            }

            if (m_state->has_pending_send()) {
                return galay::ssl::SslMachineAction<result_type>::send(
                    m_state->send_data(),
                    m_state->remaining_send_bytes());
            }

            if (m_state->try_parse_upgrade_response()) {
                return galay::ssl::SslMachineAction<result_type>::complete(m_state->take_result());
            }

            char* recv_buffer = nullptr;
            size_t recv_length = 0;
            if (!m_state->prepare_recv_window(recv_buffer, recv_length)) {
                if (!m_state->is_finished()) {
                    m_state->set_protocol_error("Upgrade response too large");
                }
                return galay::ssl::SslMachineAction<result_type>::complete(m_state->take_result());
            }

            return galay::ssl::SslMachineAction<result_type>::recv(recv_buffer, recv_length);
        }

        void on_handshake(std::expected<void, galay::ssl::SslError>) {}

        void on_recv(std::expected<Bytes, galay::ssl::SslError> result) {
            if (!result) {
                m_state->set_ssl_recv_error(result.error());
                return;
            }

            const size_t recv_bytes = result.value().size();
            if (recv_bytes == 0) {
                m_state->set_protocol_error("Connection closed");
                return;
            }

            m_state->on_bytes_received(recv_bytes);
        }

        void on_send(std::expected<size_t, galay::ssl::SslError> result) {
            if (!result) {
                m_state->set_ssl_send_error(result.error());
                return;
            }

            m_state->on_bytes_sent(result.value());
        }

        void on_shutdown(std::expected<void, galay::ssl::SslError>) {}

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
            galay::ssl::SslAwaitableBuilder<ResultType>::from_state_machine(
                upgrader->m_session->m_socket.controller(),
                &upgrader->m_session->m_socket,
                UpgradeMachine(std::move(state)))
                .build()
        );
    }

    ~WsSessionUpgradeOperation() {
        cleanup_inner_if_armed();
    }

    bool await_ready() noexcept {
        return m_ready || (m_inner_operation != nullptr && m_inner_operation->await_ready());
    }

    template<typename Promise>
    decltype(auto) await_suspend(std::coroutine_handle<Promise> handle) {
        if (m_inner_operation == nullptr) {
            cancel_bound_timeout_timer();
            return false;
        }
        forward_bound_timeout_timer(*m_inner_operation);
        m_inner_armed = true;
        return m_inner_operation->await_suspend(handle);
    }

    /**
     * @brief 暂存外层 timeout 绑定，并在 await_suspend() 中转交给 inner。
     * @param timer 定时器
     * @return 无返回值
     */
    void bind_timeout_timer(TimeoutTimer* timer) noexcept {
        SequenceAwaitableBase::bind_timeout_timer(timer);
    }

    ResultType await_resume() {
        if (!m_result.has_value()) {
            cleanup_inner_if_armed();
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
        return resume_inner();
    }

    IOTask* front() override {
        return m_inner_operation ? m_inner_operation->front() : nullptr;
    }

    const IOTask* front() const override {
        return m_inner_operation ? m_inner_operation->front() : nullptr;
    }

    void pop_front() override {
        if (m_inner_operation) {
            m_inner_operation->pop_front();
        }
    }

    bool empty() const override {
        return m_inner_operation == nullptr || m_inner_operation->empty();
    }

#ifdef USE_IOURING
    SequenceProgress prepare_for_submit() override {
        return m_inner_operation ? m_inner_operation->prepare_for_submit() : SequenceProgress::kCompleted;
    }

    SequenceProgress on_active_event(struct io_uring_cqe* cqe, GHandle handle) override {
        return m_inner_operation ? m_inner_operation->on_active_event(cqe, handle) : SequenceProgress::kCompleted;
    }
#else
    SequenceProgress prepare_for_submit(GHandle handle) override {
        return m_inner_operation ? m_inner_operation->prepare_for_submit(handle) : SequenceProgress::kCompleted;
    }

    SequenceProgress on_active_event(GHandle handle) override {
        return m_inner_operation ? m_inner_operation->on_active_event(handle) : SequenceProgress::kCompleted;
    }
#endif

public:
    std::expected<bool, galay::kernel::IOError> m_result;

private:
    ResultType resume_inner() {
        m_inner_completed = true;
        return m_inner_operation->await_resume();
    }

    void cleanup_inner_if_armed() {
        if (m_inner_operation != nullptr && m_inner_armed && !m_inner_completed) {
            m_inner_operation->on_completed();
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

private:
    WsSessionImpl(const WsSessionImpl&) = delete;
    WsSessionImpl& operator=(const WsSessionImpl&) = delete;
public:
    WsSessionImpl(WsSessionImpl&&) = delete;
    WsSessionImpl& operator=(WsSessionImpl&&) = delete;

    WsReaderImpl<SocketType>& get_reader() {
        return m_reader;
    }

    WsWriterImpl<SocketType>& get_writer() {
        return m_writer;
    }

    /**
     * @brief 执行WebSocket升级握手
     * @return 升级器对象
     */
    WsSessionUpgraderImpl<SocketType> upgrade() {
        return WsSessionUpgraderImpl<SocketType>(this);
    }

    bool is_upgraded() const {
        return m_upgraded;
    }

    // 便捷方法：发送文本消息
    auto send_text(const std::string& text, bool fin = true) {
        return m_writer.send_text(text, fin);
    }

    // 便捷方法：发送文本消息（移动语义）
    auto send_text(std::string&& text, bool fin = true) {
        return m_writer.send_text(std::move(text), fin);
    }

    // 便捷方法：发送二进制消息
    auto send_binary(const std::string& data, bool fin = true) {
        return m_writer.send_binary(data, fin);
    }

    // 便捷方法：发送二进制消息（移动语义）
    auto send_binary(std::string&& data, bool fin = true) {
        return m_writer.send_binary(std::move(data), fin);
    }

    // 便捷方法：发送Ping
    auto send_ping(const std::string& data = "") {
        return m_writer.send_ping(data);
    }

    // 便捷方法：发送Pong
    auto send_pong(const std::string& data = "") {
        return m_writer.send_pong(data);
    }

    // 便捷方法：发送Close
    auto send_close(WsCloseCode code = WsCloseCode::Normal, const std::string& reason = "") {
        return m_writer.send_close(code, reason);
    }

    // 便捷方法：接收消息
    auto get_message(std::string& message, WsOpcode& opcode) {
        return m_reader.get_message(message, opcode);
    }

    // 便捷方法：接收帧
    auto get_frame(WsFrame& frame) {
        return m_reader.get_frame(frame);
    }

    friend class detail::WsSessionUpgradeOperation<SocketType, false>;
#ifdef GALAY_SSL_FEATURE_ENABLED
    friend class detail::WsSessionUpgradeOperation<SocketType, true>;
#endif
    friend class WsSessionUpgraderImpl<SocketType>;

private:
    SocketType& m_socket;
    const WsUrl& m_url;
    RingBuffer<galay::utils::RingBufferBackendStrategy::Mmap, std::dynamic_extent> m_ring_buffer;
    WsReaderImpl<SocketType> m_reader;
    WsWriterImpl<SocketType> m_writer;
    bool m_upgraded;
};

using WsSession = WsSessionImpl<AsyncTcpSocket>;

} // namespace galay::websocket

#ifdef GALAY_SSL_FEATURE_ENABLED
#include "../../galay-ssl/async/ssl_socket.h"

namespace galay::websocket {

using WssSession = WsSessionImpl<galay::ssl::SslSocket>;

} // namespace galay::websocket
#endif

#endif // GALAY_WS_SESSION_H
