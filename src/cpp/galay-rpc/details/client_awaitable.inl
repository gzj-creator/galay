#ifndef GALAY_RPC_DETAILS_CLIENT_AWAITABLE_INL
#define GALAY_RPC_DETAILS_CLIENT_AWAITABLE_INL

namespace galay::rpc::detail
{

template<RingBufferBackendStrategy Strategy>
ExpectedRpcResponseReadState<Strategy>::ExpectedRpcResponseReadState(
    RingBuffer<Strategy, std::dynamic_extent>& ring_buffer,
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
bool ExpectedRpcResponseReadState<Strategy>::parse_from_ring_buffer()
{
    if (this->ring_buffer().readable() == 0) {
        return false;
    }

    std::array<struct iovec, 2> read_iovecs{};
    const size_t read_iovecs_count = this->ring_buffer().get_read_iovecs(read_iovecs);
    if (read_iovecs_count == 0) {
        return false;
    }

    const std::span<const iovec> read_span(read_iovecs.data(), read_iovecs_count);
    auto parse_result = try_parse_response_message(read_span,
                                                iovecs_readable_bytes(read_span),
                                                m_setting->max_message_size,
                                                *m_response);
    if (!parse_result.has_value()) {
        this->set_read_error(parse_result.error());
        return true;
    }
    if (parse_result.value() == 0) {
        return false;
    }
    if (m_response->request_id() != m_expected_request_id) {
        this->set_read_error(RpcError(RpcErrorCode::INVALID_RESPONSE,
                                    "Mismatched response request id"));
        return true;
    }

    this->ring_buffer().consume(parse_result.value());
    return true;
}

} // namespace galay::rpc::detail

namespace galay::rpc
{

template<typename SocketType, RingBufferBackendStrategy Strategy>
RecvRpcResponseChainAwaitable<SocketType, Strategy>::RecvRpcResponseChainAwaitable(
    RingBuffer<Strategy, std::dynamic_extent>& ring_buffer,
    const RpcReaderSetting& setting,
    uint32_t expected_request_id,
    RpcResponse& response)
    : m_state(std::make_shared<ReadState>(
          ring_buffer,
          setting,
          expected_request_id,
          response))
    , m_inner(AwaitableBuilder<Result>::from_state_machine(
                  nullptr,
                  detail::RpcRingBufferReadMachine<ReadState>(m_state))
                  .build())
{
}

} // namespace galay::rpc

#endif // GALAY_RPC_DETAILS_CLIENT_AWAITABLE_INL
