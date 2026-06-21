/**
 * @file rpc_channel.h
 * @brief RPC通道Facade
 * @author galay-rpc
 * @version 1.0.0
 *
 * @details RpcChannel 持有一条常驻客户端连接，并通过连接级 owner 协程维护
 *          pending request table、串行写入和响应分发。调用方通过 call() 投递请求，
 *          不直接访问底层 RpcClient/socket/ring buffer。
 */

#ifndef GALAY_RPC_CHANNEL_H
#define GALAY_RPC_CHANNEL_H

#include "rpc_client.h"
#include "../protoc/rpc_error.h"
#include "../../galay-kernel/concurrency/async_waiter.h"
#include "../../galay-kernel/concurrency/mpsc_channel.h"
#include "../../galay-kernel/core/runtime.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>

namespace galay::rpc
{

using namespace galay::kernel;
using namespace std::chrono_literals;

/**
 * @brief RPC通道状态
 */
enum class RpcChannelState : uint8_t {
    Idle,              ///< 尚未连接
    Connecting,        ///< 正在连接
    Ready,             ///< 可发起调用
    TransientFailure,  ///< 最近一次连接或I/O失败
    Shutdown           ///< 已关闭
};

/**
 * @brief RPC通道运行状态快照
 */
struct RpcChannelStatus {
    RpcChannelState state = RpcChannelState::Idle;  ///< 当前状态
    size_t outstanding = 0;                         ///< 当前未完成调用数
    size_t connections_established = 0;             ///< 当前通道生命周期内成功建立的底层连接数
};

/**
 * @brief RPC通道配置
 */
struct RpcChannelConfig {
    std::string host = "127.0.0.1";                    ///< 目标主机
    uint16_t port = 9000;                              ///< 目标端口
    size_t ring_buffer_size = kDefaultRpcRingBufferSize; ///< 常驻连接RingBuffer大小
    size_t max_message_size = 4 * 1024 * 1024;         ///< 单条响应最大body字节数
    size_t max_outstanding_requests = 1024;            ///< 最大并发未完成调用数，0表示禁止新调用
    std::chrono::milliseconds connect_timeout = 5s;    ///< 连接超时
    std::chrono::milliseconds call_timeout = 5s;       ///< 单次unary调用等待响应超时
    std::chrono::milliseconds idle_timeout = 0ms;      ///< Phase 1暂不启用，0表示不做idle回收
    std::chrono::milliseconds health_check_interval = 0ms; ///< Phase 1暂不启用，0表示不做健康探测
};

namespace detail {

struct RpcChannelCallResult {
    RpcCallResult result;
};

struct RpcChannelPending {
    std::shared_ptr<AsyncWaiter<RpcChannelCallResult>> waiter;
};

struct RpcChannelSendCommand {
    uint32_t request_id = 0;
    std::string service;
    std::string method;
    std::string payload;
    std::shared_ptr<AsyncWaiter<RpcChannelCallResult>> waiter;
};

struct RpcChannelResponseCommand {
    RpcResponse response;
};

struct RpcChannelCancelCommand {
    uint32_t request_id = 0;
    RpcError error;
};

struct RpcChannelFailCommand {
    RpcError error;
};

struct RpcChannelCloseCommand {
    RpcError error;
    std::shared_ptr<AsyncWaiter<RpcError>> waiter;
};

using RpcChannelCommand = std::variant<RpcChannelSendCommand,
                                       RpcChannelResponseCommand,
                                       RpcChannelCancelCommand,
                                       RpcChannelFailCommand,
                                       RpcChannelCloseCommand>;

struct RpcChannelCore : public std::enable_shared_from_this<RpcChannelCore> {
    explicit RpcChannelCore(RpcChannelConfig cfg)
        : config(std::move(cfg))
        , commands(1024, 64)
    {
        client_config.reader_setting.max_message_size = config.max_message_size;
        client_config.ring_buffer_size = normalizedRingBufferSize();
    }

    size_t normalizedRingBufferSize() const noexcept
    {
        return config.ring_buffer_size == 0 ? kDefaultRpcRingBufferSize : config.ring_buffer_size;
    }

    bool tryAcquireOutstanding() noexcept
    {
        size_t current = outstanding.load(std::memory_order_acquire);
        while (current < config.max_outstanding_requests) {
            if (outstanding.compare_exchange_weak(current,
                                                  current + 1,
                                                  std::memory_order_acq_rel,
                                                  std::memory_order_acquire)) {
                return true;
            }
        }
        return false;
    }

    void releaseOutstanding() noexcept
    {
        outstanding.fetch_sub(1, std::memory_order_acq_rel);
    }

    void notifyWaiter(const std::shared_ptr<AsyncWaiter<RpcChannelCallResult>>& waiter,
                      RpcCallResult result)
    {
        if (waiter) {
            (void)waiter->notify(RpcChannelCallResult{std::move(result)});
        }
    }

    void failAllPending(std::unordered_map<uint32_t, RpcChannelPending>& pending,
                        const RpcError& error)
    {
        for (auto& [request_id, entry] : pending) {
            (void)request_id;
            notifyWaiter(entry.waiter, RpcCallResult(std::unexpected(error)));
            releaseOutstanding();
        }
        pending.clear();
    }

    Task<void> readerLoop(std::shared_ptr<RpcClient> client)
    {
        auto reader = client->getReader();
        while (!closing.load(std::memory_order_acquire)) {
            RpcResponse response;
            auto recv_result = co_await reader.getResponse(response);
            if (!recv_result.has_value()) {
                (void)commands.send(RpcChannelFailCommand{
                    normalizeConnectionError(recv_result.error())});
                co_return;
            }
            response.materializePayload();
            if (!commands.send(RpcChannelResponseCommand{std::move(response)})) {
                co_return;
            }
        }
    }

    Task<void> ownerLoop(std::shared_ptr<RpcClient> client)
    {
        std::unordered_map<uint32_t, RpcChannelPending> pending;
        pending.reserve(config.max_outstanding_requests);
        auto writer = client->getWriter();
        RpcError close_error(RpcErrorCode::CONNECTION_CLOSED, "RPC channel is closed");

        for (;;) {
            auto command_result = co_await commands.recv();
            if (!command_result.has_value()) {
                close_error = RpcError::from(command_result.error(), RpcErrorCode::CONNECTION_CLOSED);
                break;
            }

            if (std::holds_alternative<RpcChannelSendCommand>(*command_result)) {
                auto command = std::move(std::get<RpcChannelSendCommand>(*command_result));
                if (closing.load(std::memory_order_acquire)) {
                    notifyWaiter(command.waiter,
                                 RpcCallResult(std::unexpected(RpcError(RpcErrorCode::CONNECTION_CLOSED,
                                                                        "RPC channel is closed"))));
                    releaseOutstanding();
                    continue;
                }

                RpcRequest request(command.request_id, command.service, command.method);
                request.callMode(RpcCallMode::UNARY);
                request.endOfStream(true);
                if (!command.payload.empty()) {
                    request.payload(command.payload.data(), command.payload.size());
                }

                auto send_result = co_await writer.sendRequest(request);
                if (!send_result.has_value()) {
                    RpcError error = normalizeConnectionError(send_result.error());
                    notifyWaiter(command.waiter, RpcCallResult(std::unexpected(error)));
                    releaseOutstanding();
                    close_error = error;
                    closing.store(true, std::memory_order_release);
                    state.store(RpcChannelState::TransientFailure, std::memory_order_release);
                    break;
                }

                pending.emplace(command.request_id,
                                RpcChannelPending{std::move(command.waiter)});
                continue;
            }

            if (std::holds_alternative<RpcChannelResponseCommand>(*command_result)) {
                auto command = std::move(std::get<RpcChannelResponseCommand>(*command_result));
                auto it = pending.find(command.response.requestId());
                if (it == pending.end()) {
                    continue;
                }
                notifyWaiter(it->second.waiter,
                             RpcCallResult(std::optional<RpcResponse>(std::move(command.response))));
                pending.erase(it);
                releaseOutstanding();
                continue;
            }

            if (std::holds_alternative<RpcChannelCancelCommand>(*command_result)) {
                auto command = std::move(std::get<RpcChannelCancelCommand>(*command_result));
                auto it = pending.find(command.request_id);
                if (it == pending.end()) {
                    continue;
                }
                notifyWaiter(it->second.waiter, RpcCallResult(std::unexpected(command.error)));
                pending.erase(it);
                releaseOutstanding();
                continue;
            }

            if (std::holds_alternative<RpcChannelFailCommand>(*command_result)) {
                auto command = std::move(std::get<RpcChannelFailCommand>(*command_result));
                close_error = normalizeConnectionError(command.error);
                closing.store(true, std::memory_order_release);
                state.store(RpcChannelState::TransientFailure, std::memory_order_release);
                break;
            }

            if (std::holds_alternative<RpcChannelCloseCommand>(*command_result)) {
                auto command = std::move(std::get<RpcChannelCloseCommand>(*command_result));
                close_error = command.error;
                closing.store(true, std::memory_order_release);
                state.store(RpcChannelState::Shutdown, std::memory_order_release);
                failAllPending(pending, close_error);
                if (command.waiter) {
                    (void)command.waiter->notify(close_error);
                }
                break;
            }
        }

        failAllPending(pending, close_error);
        closing.store(true, std::memory_order_release);
        if (state.load(std::memory_order_acquire) != RpcChannelState::Shutdown) {
            state.store(RpcChannelState::TransientFailure, std::memory_order_release);
        }
        (void)co_await client->close();
    }

    RpcChannelConfig config;
    RpcClientConfig client_config;
    MpscChannel<RpcChannelCommand> commands;
    std::atomic<RpcChannelState> state{RpcChannelState::Idle};
    std::atomic<bool> closing{false};
    std::atomic<size_t> outstanding{0};
    std::atomic<size_t> connections_established{0};
    std::atomic<uint32_t> next_request_id{1};

private:
    static RpcError normalizeConnectionError(const RpcError& error)
    {
        if (error.code() == RpcErrorCode::CONNECTION_CLOSED) {
            return error;
        }
        return RpcError(RpcErrorCode::CONNECTION_CLOSED, error.message());
    }
};

} // namespace detail

/**
 * @brief RPC通道Facade
 *
 * @details connect() 建立一条常驻连接，并启动 owner/reader 两个协程：
 *          owner 串行执行写请求、维护 pending table 并处理关闭；reader 独占响应
 *          ring buffer 读取并按 request_id 投递给 owner 分发。公共方法只通过
 *          co_await 仓库异步原语挂起，不使用阻塞锁或阻塞等待。
 *
 * @note 线程安全：call/close 可从多个 runtime 任务并发调用；共享状态通过原子量和
 *       MPSC channel 进入 owner 单写模型。
 */
class RpcChannel {
public:
    explicit RpcChannel(RpcChannelConfig config)
        : m_core(std::make_shared<detail::RpcChannelCore>(std::move(config)))
    {
    }

    RpcChannel(const RpcChannel&) = delete;
    RpcChannel& operator=(const RpcChannel&) = delete;
    RpcChannel(RpcChannel&&) = delete;
    RpcChannel& operator=(RpcChannel&&) = delete;

    /**
     * @brief 建立常驻连接并启动连接级I/O owner
     * @return 成功返回空expected；失败返回RpcError
     *
     * @note 该协程通过异步 connect 挂起，不阻塞调度线程。
     */
    Task<std::expected<void, RpcError>> connect()
    {
        if (state() == RpcChannelState::Shutdown) {
            co_return std::unexpected(RpcError(RpcErrorCode::CONNECTION_CLOSED,
                                               "RPC channel is shutdown"));
        }
        if (state() == RpcChannelState::Ready) {
            co_return std::expected<void, RpcError>{};
        }

        m_core->state.store(RpcChannelState::Connecting, std::memory_order_release);
        m_core->closing.store(false, std::memory_order_release);

        auto client = std::make_shared<RpcClient>(m_core->client_config);
        auto connected = co_await client->connect(m_core->config.host, m_core->config.port)
            .timeout(m_core->config.connect_timeout);
        if (!connected.has_value()) {
            m_core->state.store(RpcChannelState::TransientFailure, std::memory_order_release);
            co_return std::unexpected(RpcError::from(connected.error()));
        }

        m_core->connections_established.fetch_add(1, std::memory_order_acq_rel);
        auto runtime = RuntimeHandle::current();
        if (!runtime.has_value()) {
            (void)co_await client->close();
            m_core->state.store(RpcChannelState::TransientFailure, std::memory_order_release);
            co_return std::unexpected(RpcError(RpcErrorCode::INTERNAL_ERROR,
                                               "missing runtime handle"));
        }

        auto core = m_core;
        auto owner = runtime->spawn(core->ownerLoop(client));
        if (!owner.has_value()) {
            (void)co_await client->close();
            m_core->state.store(RpcChannelState::TransientFailure, std::memory_order_release);
            co_return std::unexpected(RpcError(RpcErrorCode::INTERNAL_ERROR,
                                               "failed to start RPC channel owner"));
        }
        auto reader = runtime->spawn(core->readerLoop(client));
        if (!reader.has_value()) {
            (void)m_core->commands.send(detail::RpcChannelCloseCommand{
                RpcError(RpcErrorCode::CONNECTION_CLOSED, "failed to start RPC channel reader"),
                std::make_shared<AsyncWaiter<RpcError>>()});
            m_core->state.store(RpcChannelState::TransientFailure, std::memory_order_release);
            co_return std::unexpected(RpcError(RpcErrorCode::INTERNAL_ERROR,
                                               "failed to start RPC channel reader"));
        }

        m_core->state.store(RpcChannelState::Ready, std::memory_order_release);
        co_return std::expected<void, RpcError>{};
    }

    /**
     * @brief 发起一元RPC调用
     * @param service 服务名
     * @param method 方法名
     * @param payload 请求payload视图，函数会复制到命令对象中
     * @return 成功返回RpcClient兼容的RpcCallResult；超过并发上限或通道不可用返回RpcError
     *
     * @note 该协程通过 AsyncWaiter 挂起等待 dispatcher 唤醒，不阻塞调度线程。
     */
    Task<RpcCallResult> call(const std::string& service,
                             const std::string& method,
                             std::string_view payload)
    {
        if (state() != RpcChannelState::Ready ||
            m_core->closing.load(std::memory_order_acquire)) {
            co_return RpcCallResult(std::unexpected(
                RpcError(RpcErrorCode::CONNECTION_CLOSED, "RPC channel is not ready")));
        }

        if (!m_core->tryAcquireOutstanding()) {
            co_return RpcCallResult(std::unexpected(
                RpcError(RpcErrorCode::RESOURCE_EXHAUSTED,
                         "RPC channel max outstanding requests exceeded")));
        }

        const uint32_t request_id = m_core->next_request_id.fetch_add(1, std::memory_order_relaxed);
        auto waiter = std::make_shared<AsyncWaiter<detail::RpcChannelCallResult>>();
        detail::RpcChannelSendCommand command;
        command.request_id = request_id;
        command.service = service;
        command.method = method;
        command.payload.assign(payload.data(), payload.size());
        command.waiter = waiter;

        if (!m_core->commands.send(detail::RpcChannelCommand(std::move(command)))) {
            m_core->releaseOutstanding();
            co_return RpcCallResult(std::unexpected(
                RpcError(RpcErrorCode::RESOURCE_EXHAUSTED,
                         "RPC channel command queue rejected request")));
        }

        auto waited = co_await waiter->wait().timeout(m_core->config.call_timeout);
        if (!waited.has_value()) {
            (void)m_core->commands.send(detail::RpcChannelCancelCommand{
                request_id,
                RpcError::from(waited.error(), RpcErrorCode::REQUEST_TIMEOUT)});
            co_return RpcCallResult(std::unexpected(
                RpcError::from(waited.error(), RpcErrorCode::REQUEST_TIMEOUT)));
        }

        co_return std::move(waited.value().result);
    }

    Task<RpcCallResult> call(const std::string& service,
                             const std::string& method,
                             const std::string& payload)
    {
        return call(service, method, std::string_view(payload));
    }

    Task<RpcCallResult> call(const std::string& service,
                             const std::string& method)
    {
        return call(service, method, std::string_view());
    }

    /**
     * @brief 关闭通道并唤醒所有pending调用
     * @return 成功返回空expected
     *
     * @note 幂等。关闭通过 owner 协程串行处理，并通过异步 close 关闭底层socket。
     */
    Task<std::expected<void, RpcError>> close()
    {
        const RpcChannelState previous = m_core->state.exchange(RpcChannelState::Shutdown,
                                                                std::memory_order_acq_rel);
        m_core->closing.store(true, std::memory_order_release);
        if (previous == RpcChannelState::Shutdown) {
            co_return std::expected<void, RpcError>{};
        }

        auto waiter = std::make_shared<AsyncWaiter<RpcError>>();
        const bool sent = m_core->commands.send(detail::RpcChannelCloseCommand{
            RpcError(RpcErrorCode::CONNECTION_CLOSED, "RPC channel is closed"),
            waiter});
        if (!sent) {
            co_return std::expected<void, RpcError>{};
        }
        (void)co_await waiter->wait().timeout(100ms);
        co_return std::expected<void, RpcError>{};
    }

    Task<std::expected<void, RpcError>> shutdown()
    {
        return close();
    }

    RpcChannelState state() const noexcept
    {
        return m_core->state.load(std::memory_order_acquire);
    }

    RpcChannelStatus status() const
    {
        return RpcChannelStatus{
            .state = state(),
            .outstanding = m_core->outstanding.load(std::memory_order_acquire),
            .connections_established = m_core->connections_established.load(std::memory_order_acquire)
        };
    }

private:
    std::shared_ptr<detail::RpcChannelCore> m_core;
};

} // namespace galay::rpc

#endif // GALAY_RPC_CHANNEL_H
