/**
 * @file rpc_channel.h
 * @brief RPC连接级通道
 * @author galay-rpc
 * @version 1.0.0
 *
 * @details 提供单连接的socket/ring buffer拥有者、单reader响应分发、MPSC出站队列
 *          和request_id到pending waiter的分发表。该实现不阻塞OS线程；所有I/O路径
 *          都通过仓库协程awaitable挂起恢复。
 */

#ifndef GALAY_RPC_CHANNEL_H
#define GALAY_RPC_CHANNEL_H

#include "rpc_conn.h"
#include "rpc_call.h"
#include "rpc_metrics.h"
#include "../common/rpc_log.h"
#include "../protoc/rpc_error.h"
#include "../protoc/rpc_message.h"
#include "../../galay-kernel/common/sleep.hpp"
#include "../../galay-kernel/concurrency/async_waiter.h"
#include "../../galay-kernel/concurrency/async_mutex.h"
#include "../../galay-kernel/concurrency/mpsc_channel.h"
#include "../../galay-kernel/core/scheduler.hpp"

#include <atomic>
#include <expected>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

namespace galay::rpc
{

using RpcCallResult = std::expected<std::optional<RpcResponse>, RpcError>;

/**
 * @brief RPC通道配置
 */
struct RpcChannelOptions {
    size_t max_in_flight = 1024;       ///< 单连接最大未完成请求数
    size_t max_outbound_queue = 1024;  ///< 最大待发送队列长度，0表示不允许排队
    size_t max_outbound_bytes = 16 * 1024 * 1024;  ///< 最大待发送字节数，0表示不允许写入payload
    RpcMetricCallback metrics_callback;  ///< 可选指标回调
};

/**
 * @brief 单个pending调用
 *
 * @details waiter由调用方协程等待，分发表在响应到达、timeout或close时只通知一次。
 *          对象由shared_ptr持有，保证从入队到通知期间生命周期稳定。
 */
struct RpcChannelPendingCall {
    uint32_t request_id = 0;             ///< 请求ID
    std::string service;                 ///< 指标使用的服务名
    std::string method;                  ///< 指标使用的方法名
    std::chrono::steady_clock::time_point started_at{};  ///< 调用开始时间
    AsyncWaiter<RpcCallResult> waiter;   ///< 调用完成等待器
    std::atomic<bool> completed{false};  ///< 是否已被通知
};

using RpcHeartbeatResult = std::expected<void, RpcError>;

struct RpcChannelPendingHeartbeat {
    uint32_t request_id = 0;                 ///< 心跳ID
    AsyncWaiter<RpcHeartbeatResult> waiter;  ///< 心跳完成等待器
    std::atomic<bool> completed{false};      ///< 是否已通知
};

/**
 * @brief RPC通道pending分发表
 *
 * @details 只负责request_id到pending waiter的生命周期和响应分发，不执行网络I/O。
 *          该类型不做内部锁同步；生产实现只在通道owner调度器的reader/writer loop中访问。
 */
class RpcChannelState {
public:
    explicit RpcChannelState(RpcChannelOptions options = {})
        : m_options(options)
    {
        m_pending.reserve(m_options.max_in_flight);
    }

    /**
     * @brief 注册pending请求
     * @param request_id 请求ID
     * @return 成功时返回pending waiter；失败时返回错误
     */
    std::expected<std::shared_ptr<RpcChannelPendingCall>, RpcError> registerPending(uint32_t request_id) {
        auto pending = std::make_shared<RpcChannelPendingCall>();
        pending->request_id = request_id;
        return registerPending(std::move(pending));
    }

    /**
     * @brief 注册调用方已持有的pending waiter
     * @param pending 调用方等待的pending对象
     * @return 成功时返回同一pending对象；失败时返回错误
     */
    std::expected<std::shared_ptr<RpcChannelPendingCall>, RpcError> registerPending(
        std::shared_ptr<RpcChannelPendingCall> pending) {
        if (!pending) {
            return std::unexpected(RpcError(RpcErrorCode::INVALID_REQUEST,
                                            "RPC pending call is null"));
        }
        const uint32_t request_id = pending->request_id;
        if (m_pending.size() >= m_options.max_in_flight) {
            return std::unexpected(RpcError(RpcErrorCode::RESOURCE_EXHAUSTED,
                                            "RPC channel max in-flight exceeded"));
        }
        if (m_pending.contains(request_id)) {
            return std::unexpected(RpcError(RpcErrorCode::INVALID_REQUEST,
                                            "Duplicate RPC request id"));
        }

        m_pending.emplace(request_id, pending);
        return pending;
    }

    /**
     * @brief 根据响应request_id通知对应pending waiter
     * @param response 收到的响应
     * @return 成功或INVALID_RESPONSE
     */
    std::expected<std::shared_ptr<RpcChannelPendingCall>, RpcError> dispatchResponse(RpcResponse response) {
        auto it = m_pending.find(response.requestId());
        if (it == m_pending.end()) {
            return std::unexpected(RpcError(RpcErrorCode::INVALID_RESPONSE,
                                            "Response request id has no pending call"));
        }

        auto pending = std::move(it->second);
        m_pending.erase(it);
        if (pending->completed.exchange(true, std::memory_order_acq_rel)) {
            return pending;
        }
        pending->waiter.notify(RpcCallResult(std::optional<RpcResponse>(std::move(response))));
        return pending;
    }

    /**
     * @brief 失败并移除指定pending请求
     * @param request_id 请求ID
     * @param error 要返回给调用方的错误
     * @return 请求存在时返回true
     */
    bool failPending(uint32_t request_id, const RpcError& error) {
        auto it = m_pending.find(request_id);
        if (it == m_pending.end()) {
            return false;
        }

        auto pending = std::move(it->second);
        m_pending.erase(it);
        if (pending->completed.exchange(true, std::memory_order_acq_rel)) {
            return false;
        }
        pending->waiter.notify(RpcCallResult(std::unexpected(error)));
        return true;
    }

    /**
     * @brief 失败并清空所有pending请求
     * @param error 要返回给每个pending调用的错误
     * @return 被通知的pending数量
     */
    size_t failAllPending(const RpcError& error) {
        std::vector<std::shared_ptr<RpcChannelPendingCall>> pending_calls;
        pending_calls.reserve(m_pending.size());
        for (auto& [_, pending] : m_pending) {
            pending_calls.push_back(std::move(pending));
        }
        m_pending.clear();

        for (auto& pending : pending_calls) {
            if (pending->completed.exchange(true, std::memory_order_acq_rel)) {
                continue;
            }
            pending->waiter.notify(RpcCallResult(std::unexpected(error)));
        }
        return pending_calls.size();
    }

    /// @brief 当前pending请求数量
    size_t pendingCount() const { return m_pending.size(); }
    /// @brief 指定request_id是否仍在pending表中
    bool containsPending(uint32_t request_id) const { return m_pending.contains(request_id); }
    /// @brief 通道配置
    const RpcChannelOptions& options() const { return m_options; }

private:
    RpcChannelOptions m_options;  ///< 通道配置
    std::unordered_map<uint32_t, std::shared_ptr<RpcChannelPendingCall>> m_pending;  ///< request_id到pending waiter
};

/**
 * @brief 出站队列背压计数器
 *
 * @details 只使用原子计数，不阻塞调用线程。reserve成功后调用方必须在出队发送或
 *          拒绝时调用release归还容量；reserve失败不会改变内部计数。
 */
class RpcOutboundBackpressure {
public:
    explicit RpcOutboundBackpressure(RpcChannelOptions options = {})
        : m_options(options)
    {
    }

    /**
     * @brief 预约一个出站元素及其估算字节数
     * @param bytes 出站帧估算字节数
     * @return 成功或RESOURCE_EXHAUSTED
     */
    std::expected<void, RpcError> reserve(size_t bytes) {
        if (!reserveCount()) {
            return std::unexpected(RpcError(RpcErrorCode::RESOURCE_EXHAUSTED,
                                            "RPC outbound queue limit exceeded"));
        }
        if (!reserveBytes(bytes)) {
            releaseCount();
            return std::unexpected(RpcError(RpcErrorCode::RESOURCE_EXHAUSTED,
                                            "RPC outbound byte limit exceeded"));
        }
        return {};
    }

    /// @brief 释放一次成功预约
    void release(size_t bytes) {
        releaseBytes(bytes);
        releaseCount();
    }

    /// @brief 当前预约元素数
    size_t queuedCount() const { return m_queued_count.load(std::memory_order_acquire); }
    /// @brief 当前预约字节数
    size_t queuedBytes() const { return m_queued_bytes.load(std::memory_order_acquire); }

private:
    bool reserveCount() {
        size_t current = m_queued_count.load(std::memory_order_acquire);
        while (current < m_options.max_outbound_queue) {
            if (m_queued_count.compare_exchange_weak(current,
                    current + 1,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                return true;
            }
        }
        return false;
    }

    bool reserveBytes(size_t bytes) {
        size_t current = m_queued_bytes.load(std::memory_order_acquire);
        while (bytes <= m_options.max_outbound_bytes &&
               current <= m_options.max_outbound_bytes - bytes) {
            if (m_queued_bytes.compare_exchange_weak(current,
                    current + bytes,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                return true;
            }
        }
        return false;
    }

    void releaseCount() {
        auto current = m_queued_count.load(std::memory_order_relaxed);
        while (current > 0) {
            if (m_queued_count.compare_exchange_weak(current,
                    current - 1,
                    std::memory_order_relaxed,
                    std::memory_order_relaxed)) {
                return;
            }
        }
    }

    void releaseBytes(size_t bytes) {
        auto current = m_queued_bytes.load(std::memory_order_relaxed);
        while (current >= bytes) {
            if (m_queued_bytes.compare_exchange_weak(current,
                    current - bytes,
                    std::memory_order_relaxed,
                    std::memory_order_relaxed)) {
                return;
            }
        }
        m_queued_bytes.store(0, std::memory_order_relaxed);
    }

    RpcChannelOptions m_options;
    std::atomic<size_t> m_queued_count{0};
    std::atomic<size_t> m_queued_bytes{0};
};

namespace detail {

/**
 * @brief 捕获当前协程所属调度器
 *
 * @details 该awaitable不挂起，只从promise的taskRefView读取scheduler，用于把channel
 *          reader/writer loop提交回同一个runtime-owned调度器。
 */
class CurrentSchedulerAwaitable {
public:
    bool await_ready() const noexcept { return false; }

    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> handle) noexcept {
        m_scheduler = handle.promise().taskRefView().belongScheduler();
        return false;
    }

    Scheduler* await_resume() const noexcept { return m_scheduler; }

private:
    Scheduler* m_scheduler = nullptr;
};

}  // namespace detail

/**
 * @brief RPC连接级通道实现
 *
 * @details RpcChannelImpl拥有一个socket和一个ring buffer。所有调用通过线程安全MPSC
 *          出站队列进入单writer loop；所有响应由单reader loop读取，并按request_id
 *          分发给pending waiter。Task路径不使用阻塞锁、条件变量或sleep。
 * @tparam SocketType 底层socket类型
 */
template<typename SocketType>
class RpcChannelImpl {
public:
    explicit RpcChannelImpl(const RpcReaderSetting& reader_setting = {},
                            const RpcWriterSetting& writer_setting = {},
                            size_t ring_buffer_size = kDefaultRpcRingBufferSize,
                            RpcChannelOptions options = {})
        : m_reader_setting(reader_setting)
        , m_writer_setting(writer_setting)
        , m_ring_buffer_size(ring_buffer_size == 0 ? kDefaultRpcRingBufferSize : ring_buffer_size)
        , m_state(options)
        , m_outbound_backpressure(options)
        , m_metrics(options.metrics_callback)
    {
        m_pending_heartbeats.reserve(options.max_in_flight);
    }

    RpcChannelImpl(const RpcChannelImpl&) = delete;
    RpcChannelImpl& operator=(const RpcChannelImpl&) = delete;
    RpcChannelImpl(RpcChannelImpl&&) = delete;
    RpcChannelImpl& operator=(RpcChannelImpl&&) = delete;

    /**
     * @brief 连接到远端
     * @return socket连接awaitable
     * @note 创建非阻塞socket和ring buffer；不会阻塞OS线程。
     */
    ConnectAwaitable connect(const std::string& host, uint16_t port) {
        m_socket = std::make_unique<SocketType>(IPType::IPV4);
        m_ring_buffer = std::make_unique<RingBuffer>(m_ring_buffer_size);
        m_socket->option().handleNonBlock();
        Host server_host(IPType::IPV4, host, port);
        return m_socket->connect(server_host);
    }

    /**
     * @brief 发起一元或兼容模式调用
     * @return 调用结果；错误通过RpcError返回
     *
     * @details 调用方协程只把请求发送到MPSC队列并等待pending waiter，实际socket写入
     *          和响应读取由通道单writer/single-reader loop完成。
     */
    Task<RpcCallResult> callWithMode(const std::string& service,
                                     const std::string& method,
                                     RpcCallMode mode,
                                     bool end_of_stream,
                                     const char* payload,
                                     size_t payload_len,
                                     const RpcCallOptions& options = {}) {
        if (!m_socket || !m_ring_buffer) {
            co_return RpcCallResult(std::unexpected(
                RpcError(RpcErrorCode::CONNECTION_CLOSED, "RPC channel is not connected")));
        }

        auto cancellation_token = options.cancellationToken();
        if (cancellation_token.has_value() && cancellation_token->cancelled()) {
            co_return RpcCallResult(std::unexpected(
                RpcError(RpcErrorCode::CANCELLED, "RPC call cancelled before send")));
        }

        auto* scheduler = co_await detail::CurrentSchedulerAwaitable{};
        if (scheduler == nullptr) {
            co_return RpcCallResult(std::unexpected(
                RpcError(RpcErrorCode::INTERNAL_ERROR, "RPC channel has no scheduler")));
        }
        auto loops_started = co_await ensureLoopsStarted(scheduler);
        if (!loops_started.has_value() || !loops_started.value()) {
            co_return RpcCallResult(std::unexpected(
                RpcError(RpcErrorCode::INTERNAL_ERROR, "Failed to start RPC channel loops")));
        }

        const uint32_t request_id = m_request_id.fetch_add(1, std::memory_order_relaxed);
        OutboundCall outbound;
        outbound.request = RpcRequest(request_id, service, method);
        outbound.request.callMode(mode);
        outbound.request.endOfStream(end_of_stream);
        outbound.request.metadata() = options.metadata();
        if (payload != nullptr && payload_len > 0) {
            outbound.request.payload(payload, payload_len);
        }
        outbound.reserved_bytes = estimateRequestBytes(outbound.request);
        outbound.started_at = std::chrono::steady_clock::now();
        auto reserve_result = m_outbound_backpressure.reserve(outbound.reserved_bytes);
        if (!reserve_result.has_value()) {
            co_return RpcCallResult(std::unexpected(reserve_result.error()));
        }
        outbound.pending_hint = std::make_shared<RpcChannelPendingCall>();
        outbound.pending_hint->request_id = request_id;
        outbound.pending_hint->service = service;
        outbound.pending_hint->method = method;
        outbound.pending_hint->started_at = outbound.started_at;
        auto pending = outbound.pending_hint;

        if (!m_outbound.send(std::move(outbound))) {
            m_outbound_backpressure.release(outbound.reserved_bytes);
            co_return RpcCallResult(std::unexpected(
                RpcError(RpcErrorCode::RESOURCE_EXHAUSTED, "RPC outbound queue rejected call")));
        }

        if (cancellation_token.has_value()) {
            auto* scheduler_for_cancel = scheduler;
            auto token = *cancellation_token;
            m_active_loops.fetch_add(1, std::memory_order_acq_rel);
            if (!scheduleTask(scheduler_for_cancel, cancelWatchLoop(request_id, token, pending))) {
                m_active_loops.fetch_sub(1, std::memory_order_acq_rel);
                co_return RpcCallResult(std::unexpected(
                    RpcError(RpcErrorCode::INTERNAL_ERROR, "Failed to schedule RPC cancel watcher")));
            }
        }

        auto deadline = options.effectiveDeadline(RpcClock::now());
        std::expected<RpcCallResult, IOError> wait_result;
        if (deadline.has_value()) {
            auto now = RpcClock::now();
            if (*deadline <= now) {
                pending->completed.store(true, std::memory_order_release);
                requestPendingCleanup(request_id,
                                      RpcError(RpcErrorCode::DEADLINE_EXCEEDED,
                                               "RPC deadline exceeded"));
                co_return RpcCallResult(std::unexpected(
                    RpcError(RpcErrorCode::DEADLINE_EXCEEDED, "RPC deadline exceeded")));
            }
            auto timeout = std::chrono::duration_cast<std::chrono::milliseconds>(*deadline - now);
            if (timeout.count() <= 0) {
                timeout = std::chrono::milliseconds(1);
            }
            wait_result = co_await pending->waiter.wait().timeout(timeout);
        } else {
            wait_result = co_await pending->waiter.wait();
        }
        if (!wait_result.has_value()) {
            if (IOError::contains(wait_result.error().code(), kTimeout)) {
                pending->completed.store(true, std::memory_order_release);
                requestPendingCleanup(request_id,
                                      RpcError(RpcErrorCode::DEADLINE_EXCEEDED,
                                               "RPC deadline exceeded"));
                co_return RpcCallResult(std::unexpected(
                    RpcError(RpcErrorCode::DEADLINE_EXCEEDED, "RPC deadline exceeded")));
            }
            co_return RpcCallResult(std::unexpected(
                RpcError::from(wait_result.error(), RpcErrorCode::INTERNAL_ERROR)));
        }
        co_return std::move(wait_result.value());
    }

    /**
     * @brief 发送一次HEARTBEAT ping并等待pong
     * @return pong到达时成功；发送/连接错误通过RpcError返回
     *
     * @note 使用通道单writer和单reader loop，不会与一元调用竞争socket读取。
     */
    Task<RpcHeartbeatResult> sendHeartbeat() {
        if (!m_socket || !m_ring_buffer) {
            co_return RpcHeartbeatResult(std::unexpected(
                RpcError(RpcErrorCode::CONNECTION_CLOSED, "RPC channel is not connected")));
        }

        auto* scheduler = co_await detail::CurrentSchedulerAwaitable{};
        if (scheduler == nullptr) {
            co_return RpcHeartbeatResult(std::unexpected(
                RpcError(RpcErrorCode::INTERNAL_ERROR, "Failed to start RPC channel loops")));
        }
        auto loops_started = co_await ensureLoopsStarted(scheduler);
        if (!loops_started.has_value() || !loops_started.value()) {
            co_return RpcHeartbeatResult(std::unexpected(
                RpcError(RpcErrorCode::INTERNAL_ERROR, "Failed to start RPC channel loops")));
        }

        OutboundCall outbound;
        outbound.heartbeat = true;
        outbound.heartbeat_id = m_request_id.fetch_add(1, std::memory_order_relaxed);
        outbound.heartbeat_pending = std::make_shared<RpcChannelPendingHeartbeat>();
        outbound.heartbeat_pending->request_id = outbound.heartbeat_id;
        outbound.reserved_bytes = RPC_HEADER_SIZE;
        auto pending = outbound.heartbeat_pending;

        auto reserve_result = m_outbound_backpressure.reserve(outbound.reserved_bytes);
        if (!reserve_result.has_value()) {
            co_return RpcHeartbeatResult(std::unexpected(reserve_result.error()));
        }
        if (!m_outbound.send(std::move(outbound))) {
            m_outbound_backpressure.release(outbound.reserved_bytes);
            co_return RpcHeartbeatResult(std::unexpected(
                RpcError(RpcErrorCode::RESOURCE_EXHAUSTED, "RPC outbound queue rejected heartbeat")));
        }

        auto wait_result = co_await pending->waiter.wait();
        if (!wait_result.has_value()) {
            co_return RpcHeartbeatResult(std::unexpected(
                RpcError::from(wait_result.error(), RpcErrorCode::INTERNAL_ERROR)));
        }
        co_return std::move(wait_result.value());
    }

    /// @brief 请求通道关闭并唤醒writer loop
    void requestShutdown() {
        bool expected = false;
        if (m_shutdown_requested.compare_exchange_strong(expected, true,
                                                         std::memory_order_acq_rel,
                                                         std::memory_order_acquire)) {
            OutboundCall shutdown;
            shutdown.shutdown = true;
            m_outbound.send(std::move(shutdown));
        }
    }

    /**
     * @brief 关闭底层socket并等待通道循环退出
     * @return close结果；循环等待失败时仍返回socket close错误
     *
     * @details reader/writer loop捕获this，close必须在RpcClient析构前给它们退出机会，
     *          否则局部客户端销毁后后台loop可能继续访问通道状态。等待通过sleep挂起，
     *          不阻塞OS线程。
     */
    Task<std::expected<void, IOError>> close() {
        requestShutdown();
        std::expected<void, IOError> close_result = {};
        if (m_socket) {
            close_result = co_await m_socket->close();
        }

        auto drain_result = co_await waitForBackgroundTasks(std::chrono::milliseconds(1000));
        if (!drain_result.has_value() || !drain_result.value().has_value()) {
            co_return std::unexpected(IOError(kTimeout, 0));
        }
        co_return close_result;
    }

    /// @brief 获取读取器
    RpcReaderImpl<SocketType> getReader() {
        return RpcReaderImpl<SocketType>(*m_ring_buffer, m_reader_setting, *m_socket);
    }

    /// @brief 获取写入器
    RpcWriterImpl<SocketType> getWriter() {
        return RpcWriterImpl<SocketType>(m_writer_setting, *m_socket);
    }

    /// @brief 获取底层socket
    SocketType& socket() { return *m_socket; }
    /// @brief 获取RingBuffer
    RingBuffer& ringBuffer() { return *m_ring_buffer; }
    /// @brief 获取读取配置
    const RpcReaderSetting& readerSetting() const { return m_reader_setting; }
    /// @brief 当前pending数量
    size_t pendingCount() const { return m_pending_count.load(std::memory_order_acquire); }

private:
    struct OutboundCall {
        RpcRequest request;
        std::shared_ptr<RpcChannelPendingCall> pending_hint;
        std::shared_ptr<RpcChannelPendingHeartbeat> heartbeat_pending;
        uint32_t heartbeat_id = 0;
        size_t reserved_bytes = 0;
        std::chrono::steady_clock::time_point started_at{};
        uint32_t cleanup_request_id = 0;
        std::optional<RpcError> cleanup_error;
        bool heartbeat = false;
        bool cleanup_pending = false;
        bool shutdown = false;
    };

    static size_t estimateRequestBytes(const RpcRequest& request) {
        return RPC_HEADER_SIZE + request.serializedBodySize();
    }

    Task<bool> ensureLoopsStarted(Scheduler* scheduler) {
        if (m_shutdown_requested.load(std::memory_order_acquire)) {
            co_return false;
        }
        bool expected = false;
        if (!m_loops_started.compare_exchange_strong(expected, true,
                                                     std::memory_order_acq_rel,
                                                     std::memory_order_acquire)) {
            co_return true;
        }

        m_active_loops.fetch_add(2, std::memory_order_acq_rel);
        if (!scheduleTask(scheduler, writerLoop())) {
            m_active_loops.fetch_sub(2, std::memory_order_acq_rel);
            m_loops_started.store(false, std::memory_order_release);
            co_return false;
        }
        if (!scheduleTask(scheduler, readerLoop())) {
            m_active_loops.fetch_sub(1, std::memory_order_acq_rel);
            requestShutdown();
            auto drain_result = co_await waitForBackgroundTasks(std::chrono::milliseconds(1000));
            (void)drain_result;
            m_loops_started.store(false, std::memory_order_release);
            co_return false;
        }
        co_return true;
    }

    Task<std::expected<void, IOError>> waitForBackgroundTasks(std::chrono::milliseconds timeout) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (m_active_loops.load(std::memory_order_acquire) != 0) {
            if (std::chrono::steady_clock::now() >= deadline) {
                co_return std::unexpected(IOError(kTimeout, 0));
            }
            co_await sleep(std::chrono::milliseconds(1));
        }
        co_return std::expected<void, IOError>{};
    }

    Task<void> writerLoop() {
        LoopGuard guard(*this);
        auto writer = getWriter();
        while (!m_shutdown_requested.load(std::memory_order_acquire)) {
            auto recv_result = co_await m_outbound.recv();
            if (!recv_result.has_value()) {
                continue;
            }

            auto outbound = std::move(recv_result.value());
            if (outbound.shutdown) {
                break;
            }
            if (outbound.cleanup_pending) {
                if (outbound.cleanup_error.has_value()) {
                    auto locked = co_await m_state_mutex.lock();
                    if (!locked.has_value()) {
                        requestShutdown();
                        break;
                    }
                    m_state.failPending(outbound.cleanup_request_id, *outbound.cleanup_error);
                    m_pending_count.store(m_state.pendingCount(), std::memory_order_release);
                    m_state_mutex.unlock();
                }
                continue;
            }
            if (outbound.heartbeat) {
                auto locked = co_await m_state_mutex.lock();
                if (!locked.has_value()) {
                    requestShutdown();
                    break;
                }
                m_pending_heartbeats.emplace(outbound.heartbeat_id, outbound.heartbeat_pending);
                m_state_mutex.unlock();
                auto send_result = co_await SendRawDataAwaitable<SocketType>(
                    rpcBuildHeartbeatFrame(outbound.heartbeat_id),
                    *m_socket);
                m_outbound_backpressure.release(outbound.reserved_bytes);
                if (!send_result.has_value()) {
                    auto fail_locked = co_await m_state_mutex.lock();
                    if (!fail_locked.has_value()) {
                        requestShutdown();
                        break;
                    }
                    failHeartbeat(outbound.heartbeat_id, send_result.error());
                    m_state_mutex.unlock();
                    requestShutdown();
                    break;
                }
                continue;
            }
            if (outbound.pending_hint->completed.load(std::memory_order_acquire)) {
                m_outbound_backpressure.release(outbound.reserved_bytes);
                continue;
            }

            auto locked = co_await m_state_mutex.lock();
            if (!locked.has_value()) {
                m_outbound_backpressure.release(outbound.reserved_bytes);
                requestShutdown();
                break;
            }
            auto registered = m_state.registerPending(outbound.pending_hint);
            m_pending_count.store(m_state.pendingCount(), std::memory_order_release);
            m_state_mutex.unlock();
            if (!registered.has_value()) {
                m_outbound_backpressure.release(outbound.reserved_bytes);
                outbound.pending_hint->waiter.notify(RpcCallResult(std::unexpected(registered.error())));
                continue;
            }

            auto send_result = co_await writer.sendRequest(outbound.request);
            m_outbound_backpressure.release(outbound.reserved_bytes);
            if (!send_result.has_value()) {
                auto fail_locked = co_await m_state_mutex.lock();
                if (!fail_locked.has_value()) {
                    requestShutdown();
                    break;
                }
                m_state.failPending(outbound.request.requestId(), send_result.error());
                m_pending_count.store(m_state.pendingCount(), std::memory_order_release);
                m_state_mutex.unlock();
                requestShutdown();
                break;
            }
        }
        co_return;
    }

    Task<void> readerLoop() {
        LoopGuard guard(*this);
        while (!m_shutdown_requested.load(std::memory_order_acquire)) {
            RpcHeader header;
            auto header_result = co_await GetRpcHeaderAwaitable<SocketType>(*m_ring_buffer, header, *m_socket)
                .timeout(std::chrono::milliseconds(50));
            if (!header_result.has_value()) {
                if (header_result.error().code() == RpcErrorCode::DEADLINE_EXCEEDED &&
                    m_shutdown_requested.load(std::memory_order_acquire)) {
                    break;
                }
                if (header_result.error().code() == RpcErrorCode::DEADLINE_EXCEEDED) {
                    continue;
                }
                auto locked = co_await m_state_mutex.lock();
                if (!locked.has_value()) {
                    requestShutdown();
                    break;
                }
                m_state.failAllPending(header_result.error());
                m_pending_count.store(0, std::memory_order_release);
                failAllHeartbeats(header_result.error());
                m_state_mutex.unlock();
                requestShutdown();
                break;
            }

            if (header.m_type == static_cast<uint8_t>(RpcMessageType::HEARTBEAT)) {
                auto locked = co_await m_state_mutex.lock();
                if (!locked.has_value()) {
                    requestShutdown();
                    break;
                }
                completeHeartbeat(header.m_request_id);
                m_state_mutex.unlock();
                continue;
            }

            if (header.m_type != static_cast<uint8_t>(RpcMessageType::RESPONSE)) {
                RpcError error(RpcErrorCode::INVALID_RESPONSE, "Unexpected RPC message type");
                auto locked = co_await m_state_mutex.lock();
                if (!locked.has_value()) {
                    requestShutdown();
                    break;
                }
                m_state.failAllPending(error);
                m_pending_count.store(0, std::memory_order_release);
                failAllHeartbeats(error);
                m_state_mutex.unlock();
                requestShutdown();
                break;
            }

            if (header.m_body_length > m_reader_setting.max_message_size) {
                RpcError error(RpcErrorCode::INVALID_RESPONSE, "Message too large");
                auto locked = co_await m_state_mutex.lock();
                if (!locked.has_value()) {
                    requestShutdown();
                    break;
                }
                m_state.failAllPending(error);
                m_pending_count.store(0, std::memory_order_release);
                failAllHeartbeats(error);
                m_state_mutex.unlock();
                requestShutdown();
                break;
            }

            std::vector<char> body(header.m_body_length);
            if (header.m_body_length > 0) {
                auto body_result = co_await GetRpcBodyAwaitable<SocketType>(
                    *m_ring_buffer,
                    body.data(),
                    body.size(),
                    *m_socket);
                if (!body_result.has_value()) {
                    auto locked = co_await m_state_mutex.lock();
                    if (!locked.has_value()) {
                        requestShutdown();
                        break;
                    }
                    m_state.failAllPending(body_result.error());
                    m_pending_count.store(0, std::memory_order_release);
                    failAllHeartbeats(body_result.error());
                    m_state_mutex.unlock();
                    requestShutdown();
                    break;
                }
            }

            RpcResponse response;
            response.requestId(header.m_request_id);
            response.callMode(rpcDecodeCallMode(header.m_flags));
            response.endOfStream(rpcIsEndStream(header.m_flags));
            if (!response.deserializeBody(body.data(), body.size())) {
                RpcError error(RpcErrorCode::DESERIALIZATION_ERROR, "Failed to parse response body");
                auto locked = co_await m_state_mutex.lock();
                if (!locked.has_value()) {
                    requestShutdown();
                    break;
                }
                m_state.failAllPending(error);
                m_pending_count.store(0, std::memory_order_release);
                failAllHeartbeats(error);
                m_state_mutex.unlock();
                requestShutdown();
                break;
            }

            const auto metric_status = response.errorCode();
            auto locked = co_await m_state_mutex.lock();
            if (!locked.has_value()) {
                requestShutdown();
                break;
            }
            auto dispatch_result = m_state.dispatchResponse(std::move(response));
            const size_t pending_count = m_state.pendingCount();
            m_pending_count.store(pending_count, std::memory_order_release);
            m_state_mutex.unlock();
            if (!dispatch_result.has_value()) {
                RPC_LOG_WARN("[channel] [recv] [late-or-unknown-response]",
                             "code={} error={}",
                             static_cast<int>(dispatch_result.error().code()),
                             dispatch_result.error().message());
                continue;
            }
            emitMetric(*dispatch_result.value(), metric_status, pending_count);
        }
        auto locked = co_await m_state_mutex.lock();
        if (!locked.has_value()) {
            co_return;
        }
        m_state.failAllPending(RpcError(RpcErrorCode::UNAVAILABLE, "RPC channel closed"));
        m_pending_count.store(0, std::memory_order_release);
        failAllHeartbeats(RpcError(RpcErrorCode::UNAVAILABLE, "RPC channel closed"));
        m_state_mutex.unlock();
        co_return;
    }

    bool completeHeartbeat(uint32_t request_id) {
        auto it = m_pending_heartbeats.find(request_id);
        if (it == m_pending_heartbeats.end()) {
            return false;
        }
        auto pending = std::move(it->second);
        m_pending_heartbeats.erase(it);
        if (pending->completed.exchange(true, std::memory_order_acq_rel)) {
            return false;
        }
        pending->waiter.notify(RpcHeartbeatResult{});
        return true;
    }

    bool failHeartbeat(uint32_t request_id, const RpcError& error) {
        auto it = m_pending_heartbeats.find(request_id);
        if (it == m_pending_heartbeats.end()) {
            return false;
        }
        auto pending = std::move(it->second);
        m_pending_heartbeats.erase(it);
        if (pending->completed.exchange(true, std::memory_order_acq_rel)) {
            return false;
        }
        pending->waiter.notify(RpcHeartbeatResult(std::unexpected(error)));
        return true;
    }

    void failAllHeartbeats(const RpcError& error) {
        std::vector<std::shared_ptr<RpcChannelPendingHeartbeat>> pending;
        pending.reserve(m_pending_heartbeats.size());
        for (auto& [_, heartbeat] : m_pending_heartbeats) {
            pending.push_back(std::move(heartbeat));
        }
        m_pending_heartbeats.clear();
        for (auto& heartbeat : pending) {
            if (!heartbeat->completed.exchange(true, std::memory_order_acq_rel)) {
                heartbeat->waiter.notify(RpcHeartbeatResult(std::unexpected(error)));
            }
        }
    }

    void emitMetric(const RpcChannelPendingCall& pending, RpcErrorCode status, size_t pending_count) {
        RpcMetricEvent event;
        event.service = pending.service;
        event.method = pending.method;
        event.pending_calls = pending_count;
        event.latency = std::chrono::steady_clock::now() - pending.started_at;
        event.status = status;
        m_metrics.emit(event);
    }

    Task<void> cancelWatchLoop(uint32_t request_id,
                               RpcCancellationToken token,
                               std::shared_ptr<RpcChannelPendingCall> pending) {
        LoopGuard guard(*this);
        while (!m_shutdown_requested.load(std::memory_order_acquire)) {
            if (token.cancelled()) {
                if (!pending->completed.exchange(true, std::memory_order_acq_rel)) {
                    pending->waiter.notify(RpcCallResult(std::unexpected(
                        RpcError(RpcErrorCode::CANCELLED, "RPC call cancelled"))));
                }
                if (!m_shutdown_requested.load(std::memory_order_acquire)) {
                    requestPendingCleanup(request_id,
                                          RpcError(RpcErrorCode::CANCELLED, "RPC call cancelled"));
                }
                co_return;
            }
            if (pending->completed.load(std::memory_order_acquire)) {
                co_return;
            }
            co_await sleep(std::chrono::milliseconds(1));
        }
        co_return;
    }

    void requestPendingCleanup(uint32_t request_id, RpcError error) {
        OutboundCall cleanup;
        cleanup.cleanup_pending = true;
        cleanup.cleanup_request_id = request_id;
        cleanup.cleanup_error = std::move(error);
        m_outbound.send(std::move(cleanup));
    }

private:
    class LoopGuard {
    public:
        explicit LoopGuard(RpcChannelImpl& channel)
            : m_channel(channel)
        {
        }

        ~LoopGuard()
        {
            m_channel.m_active_loops.fetch_sub(1, std::memory_order_acq_rel);
        }

        LoopGuard(const LoopGuard&) = delete;
        LoopGuard& operator=(const LoopGuard&) = delete;

    private:
        RpcChannelImpl& m_channel;
    };

    std::unique_ptr<SocketType> m_socket;       ///< 底层socket
    std::unique_ptr<RingBuffer> m_ring_buffer;  ///< 读取ring buffer
    RpcReaderSetting m_reader_setting;         ///< 读取配置
    RpcWriterSetting m_writer_setting;         ///< 写入配置
    size_t m_ring_buffer_size;                 ///< ring buffer大小
    RpcChannelState m_state;                   ///< pending分发表
    std::atomic<size_t> m_pending_count{0};    ///< 无锁诊断用pending数量快照
    AsyncMutex m_state_mutex;                  ///< 串行化pending/heartbeat表访问
    RpcOutboundBackpressure m_outbound_backpressure;  ///< 出站队列背压计数
    RpcMetricsSink m_metrics;                  ///< 指标回调
    MpscChannel<OutboundCall> m_outbound;      ///< 线程安全出站队列
    std::unordered_map<uint32_t, std::shared_ptr<RpcChannelPendingHeartbeat>> m_pending_heartbeats;  ///< heartbeat等待表
    std::atomic<uint32_t> m_request_id{0};     ///< 请求ID生成器
    std::atomic<bool> m_loops_started{false};  ///< reader/writer loop是否已启动
    std::atomic<bool> m_shutdown_requested{false};  ///< 是否请求关闭
    std::atomic<size_t> m_active_loops{0};      ///< 仍在访问通道状态的后台loop数量
};

using RpcChannel = RpcChannelImpl<TcpSocket>;

}  // namespace galay::rpc

#endif  // GALAY_RPC_CHANNEL_H
