#ifndef GALAY_HTTP2_DETAILS_H2C_CLIENT_AWAITABLE_INL
#define GALAY_HTTP2_DETAILS_H2C_CLIENT_AWAITABLE_INL

namespace galay::http2
{

template<RingBufferBackendStrategy Strategy>
H2cUpgradeAwaitable<Strategy>::H2cUpgradeAwaitable(
    H2cClient<Strategy>& client,
    const std::string& path)
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
            H2cUpgradeMachine<Strategy>(client, path))
            .build());
}

template<RingBufferBackendStrategy Strategy>
H2cUpgradeAwaitable<Strategy>::~H2cUpgradeAwaitable()
{
    cleanupInnerIfArmed();
}

template<RingBufferBackendStrategy Strategy>
bool H2cUpgradeAwaitable<Strategy>::await_ready() const noexcept
{
    return m_ready || (m_inner_operation != nullptr && m_inner_operation->await_ready());
}

template<RingBufferBackendStrategy Strategy>
template<typename Promise>
decltype(auto) H2cUpgradeAwaitable<Strategy>::await_suspend(
    std::coroutine_handle<Promise> handle)
{
    if (m_inner_operation == nullptr) {
        return false;
    }
    m_scheduler = handle.promise().taskRefView().belongScheduler();
    m_inner_armed = true;
    return m_inner_operation->await_suspend(handle);
}

template<RingBufferBackendStrategy Strategy>
void H2cUpgradeAwaitable<Strategy>::markTimeout()
{
    m_result = std::unexpected(IOError(kTimeout, 0));
    if (m_inner_operation != nullptr) {
        m_inner_operation->markTimeout();
    }
}

template<RingBufferBackendStrategy Strategy>
galay::kernel::SequenceAwaitableBase::IOTask*
H2cUpgradeAwaitable<Strategy>::front()
{
    return m_inner_operation ? m_inner_operation->front() : nullptr;
}

template<RingBufferBackendStrategy Strategy>
const galay::kernel::SequenceAwaitableBase::IOTask*
H2cUpgradeAwaitable<Strategy>::front() const
{
    return m_inner_operation ? m_inner_operation->front() : nullptr;
}

template<RingBufferBackendStrategy Strategy>
void H2cUpgradeAwaitable<Strategy>::popFront()
{
    if (m_inner_operation) {
        m_inner_operation->popFront();
    }
}

template<RingBufferBackendStrategy Strategy>
bool H2cUpgradeAwaitable<Strategy>::empty() const
{
    return m_inner_operation == nullptr || m_inner_operation->empty();
}

#ifdef USE_IOURING
template<RingBufferBackendStrategy Strategy>
SequenceProgress H2cUpgradeAwaitable<Strategy>::prepareForSubmit()
{
    return m_inner_operation ? m_inner_operation->prepareForSubmit()
                             : SequenceProgress::kCompleted;
}

template<RingBufferBackendStrategy Strategy>
SequenceProgress H2cUpgradeAwaitable<Strategy>::onActiveEvent(
    struct io_uring_cqe* cqe,
    GHandle handle)
{
    return m_inner_operation ? m_inner_operation->onActiveEvent(cqe, handle)
                             : SequenceProgress::kCompleted;
}
#else
template<RingBufferBackendStrategy Strategy>
SequenceProgress H2cUpgradeAwaitable<Strategy>::prepareForSubmit(GHandle handle)
{
    return m_inner_operation ? m_inner_operation->prepareForSubmit(handle)
                             : SequenceProgress::kCompleted;
}

template<RingBufferBackendStrategy Strategy>
SequenceProgress H2cUpgradeAwaitable<Strategy>::onActiveEvent(GHandle handle)
{
    return m_inner_operation ? m_inner_operation->onActiveEvent(handle)
                             : SequenceProgress::kCompleted;
}
#endif

template<RingBufferBackendStrategy Strategy>
typename H2cUpgradeAwaitable<Strategy>::ResultType
H2cUpgradeAwaitable<Strategy>::resumeInner()
{
    m_inner_completed = true;
    return m_inner_operation->await_resume();
}

template<RingBufferBackendStrategy Strategy>
void H2cUpgradeAwaitable<Strategy>::cleanupInnerIfArmed()
{
    if (m_inner_operation != nullptr && m_inner_armed && !m_inner_completed) {
        m_inner_operation->onCompleted();
        m_inner_completed = true;
    }
}

template<RingBufferBackendStrategy Strategy>
void H2cUpgradeAwaitable<Strategy>::discardTransport(H2cClient<Strategy>& client)
{
    if (client.m_conn != nullptr) {
        if (client.m_conn->socket().handle().fd >= 0) {
            const int close_result = ::close(client.m_conn->socket().handle().fd);
            if (close_result != 0) {
                client.m_upgrade_result = std::unexpected(Http2Error(
                    Http2ErrorCode::ConnectError,
                    "failed to close upgraded h2c connection fd"));
            }
        }
        client.m_conn.reset();
    }
    if (client.m_socket != nullptr && client.m_socket->handle().fd >= 0) {
        const int close_result = ::close(client.m_socket->handle().fd);
        if (close_result != 0) {
            client.m_upgrade_result = std::unexpected(Http2Error(
                Http2ErrorCode::ConnectError,
                "failed to close h2c socket fd"));
        }
    }
    client.m_socket.reset();
    client.m_ring_buffer.reset();
    client.m_upgraded = false;
}

template<RingBufferBackendStrategy Strategy>
bool H2cUpgradeAwaitable<Strategy>::finalizeTransport(
    H2cClient<Strategy>& client,
    Scheduler* scheduler)
{
    if (scheduler == nullptr || client.m_socket == nullptr || client.m_ring_buffer == nullptr) {
        return false;
    }

    client.m_conn = std::make_unique<Http2ConnImpl<TcpSocket, Strategy>>(
        std::move(*client.m_socket), std::move(*client.m_ring_buffer));
    auto local_settings = Http2Conn::makeSettingsFrameFromConfig(client.m_config, 0);
    if (client.m_conn->applyLocalSettings(local_settings) != Http2ErrorCode::NoError) {
        return false;
    }
    if (client.m_pending_peer_settings.has_value() &&
        client.m_conn->applyPeerSettings(*client.m_pending_peer_settings) != Http2ErrorCode::NoError) {
        return false;
    }
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
    client.m_pending_peer_settings.reset();
    client.m_upgraded = true;
    client.m_upgrade_result = true;
    return true;
}

template<RingBufferBackendStrategy Strategy>
Http2Error H2cUpgradeAwaitable<Strategy>::translateIoError(const IOError& error)
{
    if (IOError::contains(error.code(), kTimeout)) {
        return Http2Error(Http2ErrorCode::ConnectError, "upgrade timeout");
    }
    if (IOError::contains(error.code(), kDisconnectError)) {
        return Http2Error(Http2ErrorCode::ConnectError, error.message());
    }
    return Http2Error(Http2ErrorCode::InternalError, error.message());
}

template<RingBufferBackendStrategy Strategy>
std::expected<bool, Http2Error> H2cUpgradeAwaitable<Strategy>::await_resume()
{
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
        return fail(Http2Error(Http2ErrorCode::InternalError,
                               "failed to finalize h2c transport"));
    }
    return result;
}

template<RingBufferBackendStrategy Strategy>
H2cUpgradeAwaitable<Strategy> H2cClient<Strategy>::upgrade(const std::string& path)
{
    return H2cUpgradeAwaitable<Strategy>(*this, path);
}

} // namespace galay::http2

#endif // GALAY_HTTP2_DETAILS_H2C_CLIENT_AWAITABLE_INL
