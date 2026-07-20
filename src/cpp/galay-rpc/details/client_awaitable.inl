#ifndef GALAY_RPC_DETAILS_CLIENT_AWAITABLE_INL
#define GALAY_RPC_DETAILS_CLIENT_AWAITABLE_INL

namespace galay::rpc::detail
{

template<RingBufferBackendStrategy Strategy>
ExpectedRpcResponseReadState<Strategy>::ExpectedRpcResponseReadState(
    RingBuffer<Strategy>& ring_buffer,
    const RpcReaderSetting& setting,
    uint32_t expected_request_id,
    RpcResponse& response)
    : Base(ring_buffer)
    , m_setting(&setting)
    , m_response(&response)
    , m_expected_request_id(expected_request_id)
{
}

template<RingBufferBackendStrategy Strategy>
bool ExpectedRpcResponseReadState<Strategy>::parseFromRingBuffer()
{
    if (this->ringBuffer().readable() == 0) {
        return false;
    }

    std::array<struct iovec, 2> read_iovecs{};
    const size_t read_iovecs_count = this->ringBuffer().getReadIovecs(read_iovecs);
    if (read_iovecs_count == 0) {
        return false;
    }

    const std::span<const iovec> read_span(read_iovecs.data(), read_iovecs_count);
    auto parse_result = tryParseResponseMessage(read_span,
                                                iovecsReadableBytes(read_span),
                                                m_setting->max_message_size,
                                                *m_response);
    if (!parse_result.has_value()) {
        this->setReadError(parse_result.error());
        return true;
    }
    if (parse_result.value() == 0) {
        return false;
    }
    if (m_response->requestId() != m_expected_request_id) {
        this->setReadError(RpcError(RpcErrorCode::INVALID_RESPONSE,
                                    "Mismatched response request id"));
        return true;
    }

    this->ringBuffer().consume(parse_result.value());
    return true;
}

} // namespace galay::rpc::detail

namespace galay::rpc
{

template<typename SocketType, RingBufferBackendStrategy Strategy>
RecvRpcResponseChainAwaitable<SocketType, Strategy>::RecvRpcResponseChainAwaitable(
    RingBuffer<Strategy>& ring_buffer,
    const RpcReaderSetting& setting,
    uint32_t expected_request_id,
    RpcResponse& response)
    : m_state(std::make_shared<ReadState>(
          ring_buffer,
          setting,
          expected_request_id,
          response))
    , m_inner(AwaitableBuilder<Result>::fromStateMachine(
                  nullptr,
                  detail::RpcRingBufferReadMachine<ReadState>(m_state))
                  .build())
{
}

template<typename SocketType, RingBufferBackendStrategy Strategy>
bool RecvRpcResponseChainAwaitable<SocketType, Strategy>::await_ready()
{
    return m_inner.await_ready();
}

template<typename SocketType, RingBufferBackendStrategy Strategy>
template<typename Promise>
bool RecvRpcResponseChainAwaitable<SocketType, Strategy>::await_suspend(
    std::coroutine_handle<Promise> handle)
{
    return m_inner.await_suspend(handle);
}

template<typename SocketType, RingBufferBackendStrategy Strategy>
typename RecvRpcResponseChainAwaitable<SocketType, Strategy>::Result
RecvRpcResponseChainAwaitable<SocketType, Strategy>::await_resume()
{
    return m_inner.await_resume();
}

template<typename SocketType, RingBufferBackendStrategy Strategy>
void RecvRpcResponseChainAwaitable<SocketType, Strategy>::markTimeout()
{
    m_inner.markTimeout();
}

} // namespace galay::rpc

#endif // GALAY_RPC_DETAILS_CLIENT_AWAITABLE_INL
