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
        AwaitableBuilder<ResultType>::from_state_machine(
            client.m_socket->controller(),
            H2cUpgradeMachine<Strategy>(client, path))
            .build());
}

template<RingBufferBackendStrategy Strategy>
H2cUpgradeAwaitable<Strategy>::~H2cUpgradeAwaitable()
{
    cleanup_inner_if_armed();
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
        cancel_bound_timeout_timer();
        return false;
    }
    forward_bound_timeout_timer(*m_inner_operation);
    m_scheduler = handle.promise().task_ref_view().belong_scheduler();
    m_inner_armed = true;
    return m_inner_operation->await_suspend(handle);
}

template<RingBufferBackendStrategy Strategy>
void H2cUpgradeAwaitable<Strategy>::bind_timeout_timer(TimeoutTimer* timer) noexcept
{
    SequenceAwaitableBase::bind_timeout_timer(timer);
}

template<RingBufferBackendStrategy Strategy>
void H2cUpgradeAwaitable<Strategy>::mark_timeout()
{
    m_result = std::unexpected(IOError(kTimeout, 0));
    if (m_inner_operation != nullptr) {
        m_inner_operation->mark_timeout();
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
void H2cUpgradeAwaitable<Strategy>::pop_front()
{
    if (m_inner_operation) {
        m_inner_operation->pop_front();
    }
}

template<RingBufferBackendStrategy Strategy>
bool H2cUpgradeAwaitable<Strategy>::empty() const
{
    return m_inner_operation == nullptr || m_inner_operation->empty();
}

#ifdef USE_IOURING
template<RingBufferBackendStrategy Strategy>
SequenceProgress H2cUpgradeAwaitable<Strategy>::prepare_for_submit()
{
    return m_inner_operation ? m_inner_operation->prepare_for_submit()
                             : SequenceProgress::kCompleted;
}

template<RingBufferBackendStrategy Strategy>
SequenceProgress H2cUpgradeAwaitable<Strategy>::on_active_event(
    struct io_uring_cqe* cqe,
    GHandle handle)
{
    return m_inner_operation ? m_inner_operation->on_active_event(cqe, handle)
                             : SequenceProgress::kCompleted;
}
#else
template<RingBufferBackendStrategy Strategy>
SequenceProgress H2cUpgradeAwaitable<Strategy>::prepare_for_submit(GHandle handle)
{
    return m_inner_operation ? m_inner_operation->prepare_for_submit(handle)
                             : SequenceProgress::kCompleted;
}

template<RingBufferBackendStrategy Strategy>
SequenceProgress H2cUpgradeAwaitable<Strategy>::on_active_event(GHandle handle)
{
    return m_inner_operation ? m_inner_operation->on_active_event(handle)
                             : SequenceProgress::kCompleted;
}
#endif

template<RingBufferBackendStrategy Strategy>
typename H2cUpgradeAwaitable<Strategy>::ResultType
H2cUpgradeAwaitable<Strategy>::resume_inner()
{
    m_inner_completed = true;
    return m_inner_operation->await_resume();
}

template<RingBufferBackendStrategy Strategy>
void H2cUpgradeAwaitable<Strategy>::cleanup_inner_if_armed()
{
    if (m_inner_operation != nullptr && m_inner_armed && !m_inner_completed) {
        m_inner_operation->on_completed();
        m_inner_completed = true;
    }
}

template<RingBufferBackendStrategy Strategy>
void H2cUpgradeAwaitable<Strategy>::discard_transport(H2cClient<Strategy>& client)
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
bool H2cUpgradeAwaitable<Strategy>::finalize_transport(
    H2cClient<Strategy>& client,
    Scheduler* scheduler)
{
    if (scheduler == nullptr || client.m_socket == nullptr || client.m_ring_buffer == nullptr) {
        return false;
    }

    client.m_conn = std::make_unique<Http2ConnImpl<AsyncTcpSocket, Strategy>>(
        std::move(*client.m_socket), std::move(*client.m_ring_buffer));
    auto local_settings = Http2Conn::make_settings_frame_from_config(client.m_config, 0);
    if (client.m_conn->apply_local_settings(local_settings) != Http2ErrorCode::NoError) {
        return false;
    }
    if (client.m_pending_peer_settings.has_value() &&
        client.m_conn->apply_peer_settings(*client.m_pending_peer_settings) != Http2ErrorCode::NoError) {
        return false;
    }
    client.m_conn->runtime_config().from(client.m_config);
    client.m_conn->mark_settings_sent();
    client.m_conn->set_is_client(true);
    client.m_conn->init_stream_manager();

    auto* manager = client.m_conn->stream_manager();
    if (manager == nullptr) {
        return false;
    }
    if (!manager->start_with_scheduler(
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
Http2Error H2cUpgradeAwaitable<Strategy>::translate_io_error(const IOError& error)
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
        discard_transport(*m_client);
        m_client->m_upgrade_result = std::unexpected(error);
        return std::unexpected(std::move(error));
    };

    if (!m_result.has_value()) {
        cleanup_inner_if_armed();
        return fail(translate_io_error(m_result.error()));
    }
    if (m_error.has_value()) {
        cleanup_inner_if_armed();
        return fail(*m_error);
    }
    if (m_ready) {
        return fail(Http2Error(Http2ErrorCode::ConnectError, "not connected"));
    }
    if (m_inner_operation != nullptr && m_inner_operation->m_error.has_value()) {
        cleanup_inner_if_armed();
        return fail(translate_io_error(*m_inner_operation->m_error));
    }

    auto result = resume_inner();
    if (!result) {
        return fail(result.error());
    }
    if (!finalize_transport(*m_client, m_scheduler)) {
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
