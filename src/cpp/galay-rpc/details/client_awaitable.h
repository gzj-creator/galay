#ifndef GALAY_RPC_DETAILS_CLIENT_AWAITABLE_H
#define GALAY_RPC_DETAILS_CLIENT_AWAITABLE_H

/**
 * @file client_awaitable.h
 * @brief RPC 客户端自定义等待体声明
 * @details 保持 rpc_client.h 只承载客户端公开接口与配置。
 */

#include "../kernel/rpc_client.h"

namespace galay::rpc
{

namespace detail
{

/**
 * @brief 期望特定 request_id 的 RPC 响应读取状态
 */
template<RingBufferBackendStrategy Strategy = RingBufferBackendStrategy::Mmap>
class ExpectedRpcResponseReadState
    : public RpcRingBufferReadStateBase<RpcAwaitableResult, Strategy>
{
public:
    using Base = RpcRingBufferReadStateBase<RpcAwaitableResult, Strategy>;

    ExpectedRpcResponseReadState(RingBuffer<Strategy>& ring_buffer,
                                 const RpcReaderSetting& setting,
                                 uint32_t expected_request_id,
                                 RpcResponse& response);

    bool parseFromRingBuffer();

private:
    const RpcReaderSetting* m_setting = nullptr; ///< 读取配置
    RpcResponse* m_response = nullptr;           ///< 输出响应对象
    uint32_t m_expected_request_id = 0;          ///< 期望的请求 ID
};

} // namespace detail

/**
 * @brief 接收 RPC 响应的链式等待体
 * @note 等待体通过状态机挂起协程，不阻塞调用线程；错误使用 RpcAwaitableResult 显式返回。
 */
template<typename SocketType,
         RingBufferBackendStrategy Strategy = RingBufferBackendStrategy::Mmap>
class RecvRpcResponseChainAwaitable
    : public TimeoutSupport<RecvRpcResponseChainAwaitable<SocketType, Strategy>>
{
public:
    using Result = detail::RpcAwaitableResult;
    using ReadState = detail::ExpectedRpcResponseReadState<Strategy>;

    RecvRpcResponseChainAwaitable(RingBuffer<Strategy>& ring_buffer,
                                  const RpcReaderSetting& setting,
                                  uint32_t expected_request_id,
                                  RpcResponse& response);
    RecvRpcResponseChainAwaitable(RecvRpcResponseChainAwaitable&&) noexcept = default;
    RecvRpcResponseChainAwaitable& operator=(RecvRpcResponseChainAwaitable&&) noexcept = default;
    RecvRpcResponseChainAwaitable(const RecvRpcResponseChainAwaitable&) = delete;
    RecvRpcResponseChainAwaitable& operator=(const RecvRpcResponseChainAwaitable&) = delete;

    bool await_ready();

    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> handle);

    Result await_resume();
    void markTimeout();

private:
    using InnerAwaitable =
        StateMachineAwaitable<detail::RpcRingBufferReadMachine<ReadState>>;

    std::shared_ptr<ReadState> m_state; ///< 读取状态
    InnerAwaitable m_inner;             ///< 内部状态机等待体
};

} // namespace galay::rpc

#include "client_awaitable.inl"

#endif // GALAY_RPC_DETAILS_CLIENT_AWAITABLE_H
