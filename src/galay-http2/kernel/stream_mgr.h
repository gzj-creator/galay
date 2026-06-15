/**
 * @file stream_mgr.h
 * @brief HTTP/2 流管理器，管理多路复用流的生命周期
 * @author galay-http
 * @version 1.0.0
 *
 * @details 提供 Http2StreamManagerImpl 模板类，管理 HTTP/2 连接上的所有流，
 *          包括流的创建、销毁、优先级调度和流 ID 分配。
 */

#ifndef GALAY_HTTP2_STREAM_MANAGER_H
#define GALAY_HTTP2_STREAM_MANAGER_H

#include "http2_conn.h"
#include "http2_stream.h"
#include "galay-http2/protoc/http2_base.h"
#include "galay-http2/protoc/http2_frame.h"
#include "galay-http/common/iovec_utils.h"
#include "galay-kernel/concurrency/async_waiter.h"
#include "galay-kernel/concurrency/mpsc_channel.h"
#include "galay-kernel/common/sleep.hpp"
#include <memory>
#include <queue>
#include <string>
#include <vector>
#include <optional>
#include <coroutine>
#include <functional>
#include <type_traits>
#include <atomic>
#include <chrono>
#include <array>
#include <cstring>
#include <algorithm>
#include <deque>
#include <limits>

namespace galay::http2
{

using namespace galay::kernel;

/**
 * @brief 按优先级排序的流比较器
 * weight 越大优先级越高（大顶堆）
 */
struct StreamPriorityCompare {
    bool operator()(const Http2Stream::ptr& a, const Http2Stream::ptr& b) const {
        return a->weight() < b->weight();
    }
};

/**
 * @brief 待处理的连接级动作（由非协程函数标记，由主循环执行）
 */
struct PendingAction {
    enum class Type {
        SendGoaway,
        SendRstStream,
        SendWindowUpdate
    };
    Type type;
    uint32_t stream_id = 0;
    Http2ErrorCode error_code = Http2ErrorCode::NoError;
    uint32_t increment = 0;
};

/**
 * @brief 用户流处理器类型
 */
using Http2StreamHandler = std::function<Task<void>(Http2Stream::ptr)>;

class StartDetachedTaskAwaitable {
public:
    explicit StartDetachedTaskAwaitable(Task<void>&& task) noexcept
        : m_task(std::move(task))
    {
    }

    bool await_ready() const noexcept {
        return !m_task.isValid();
    }

    template<typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> handle) {
        auto* scheduler = handle.promise().taskRefView().belongScheduler();
        if (scheduler == nullptr) {
            m_scheduled = false;
            return false;
        }
        if (!scheduleTask(scheduler, std::move(m_task))) {
            m_scheduled = false;
            return false;
        }
        m_scheduled = true;
        return false;
    }

    bool await_resume() const noexcept { return m_scheduled; }

private:
    Task<void> m_task;
    bool m_scheduled = true;
};

inline StartDetachedTaskAwaitable startDetachedTask(Task<void> task) {
    return StartDetachedTaskAwaitable(std::move(task));
}

class Http2ActiveStreamBatch {
public:
    void mark(const Http2Stream::ptr& stream, Http2StreamEvent events) {
        if (!stream || events == Http2StreamEvent::None) {
            return;
        }
        stream->m_pending_events |= events;
        if (stream->m_active_queued) {
            return;
        }
        stream->m_active_queued = true;
        m_ready.push_back(stream);
    }

    std::vector<Http2Stream::ptr> takeReady() {
        std::vector<Http2Stream::ptr> ready;
        ready.swap(m_ready);
        return ready;
    }

    bool empty() const {
        return m_ready.empty();
    }

private:
    std::vector<Http2Stream::ptr> m_ready;
};

class Http2ActiveStreamMailbox {
public:
    using Batch = std::vector<Http2Stream::ptr>;
    using BatchResult = std::optional<Batch>;

    class RecvBatchAwaitable {
    public:
        RecvBatchAwaitable(Http2ActiveStreamMailbox* mailbox, size_t max_count)
            : m_mailbox(mailbox)
            , m_max_count(std::max<size_t>(max_count, 1))
        {
        }

        bool await_ready() const noexcept {
            return m_mailbox->m_closed || !m_mailbox->m_batches.empty();
        }

        template<typename Handle>
        bool await_suspend(Handle handle) noexcept {
            if (m_mailbox->m_closed || !m_mailbox->m_batches.empty()) {
                return false;
            }
            m_mailbox->m_waiter = Waker(handle);
            m_mailbox->m_hasWaiter = true;
            return true;
        }

        BatchResult await_resume() {
            return m_mailbox->popBatch(m_max_count);
        }

    private:
        Http2ActiveStreamMailbox* m_mailbox;
        size_t m_max_count;
    };

    void sendBatch(Batch&& batch) {
        if (m_closed || batch.empty()) {
            return;
        }
        m_batches.push_back(std::move(batch));
        wakeWaiter();
    }

    RecvBatchAwaitable recvBatch(
        size_t max_count = galay::kernel::UnsafeChannel<Http2Stream::ptr>::DEFAULT_BATCH_SIZE) {
        return RecvBatchAwaitable(this, max_count);
    }

    void close() {
        if (m_closed) {
            return;
        }
        m_closed = true;
        wakeWaiter();
    }

    void reset() {
        m_batches.clear();
        m_closed = false;
        m_hasWaiter = false;
        m_waiter = {};
    }

private:
    friend class RecvBatchAwaitable;

    BatchResult popBatch(size_t max_count) {
        if (m_batches.empty()) {
            if (m_closed) {
                return std::nullopt;
            }
            return Batch{};
        }

        auto& front = m_batches.front();
        if (front.size() <= max_count) {
            Batch batch = std::move(front);
            m_batches.pop_front();
            return batch;
        }

        Batch batch;
        batch.reserve(max_count);
        auto split = front.begin() + static_cast<std::ptrdiff_t>(max_count);
        std::move(front.begin(), split, std::back_inserter(batch));
        front.erase(front.begin(), split);
        return batch;
    }

    void wakeWaiter() {
        if (!m_hasWaiter) {
            return;
        }
        m_hasWaiter = false;
        auto waiter = std::move(m_waiter);
        m_waiter = {};
        waiter.wakeUp();
    }

    std::deque<Batch> m_batches;
    bool m_closed = false;
    bool m_hasWaiter = false;
    Waker m_waiter;
};

class Http2ConnContext {
public:
    using ActiveStreamBatch = std::optional<std::vector<Http2Stream::ptr>>;

    class GetActiveStreamsAwaitable {
    public:
        GetActiveStreamsAwaitable(Http2ConnContext* ctx, size_t max_count)
            : m_ctx(ctx)
            , m_recv_awaitable(ctx->m_mailbox->recvBatch(max_count))
        {
        }

        bool await_ready() const noexcept {
            return m_ctx->m_closed || m_recv_awaitable.await_ready();
        }

        template<typename Handle>
        bool await_suspend(Handle handle) {
            if (m_ctx->m_closed) {
                return false;
            }
            return m_recv_awaitable.await_suspend(handle);
        }

        ActiveStreamBatch await_resume() {
            if (m_ctx->m_closed) {
                return std::nullopt;
            }

            auto result = m_recv_awaitable.await_resume();
            if (!result) {
                m_ctx->m_closed = true;
                return std::nullopt;
            }
            return std::move(result.value());
        }

    private:
        Http2ConnContext* m_ctx;
        Http2ActiveStreamMailbox::RecvBatchAwaitable m_recv_awaitable;
    };

    explicit Http2ConnContext(Http2ActiveStreamMailbox& mailbox)
        : m_mailbox(&mailbox)
    {
    }

    Http2ConnContext(const Http2ConnContext&) = delete;
    Http2ConnContext& operator=(const Http2ConnContext&) = delete;
    Http2ConnContext(Http2ConnContext&&) = delete;
    Http2ConnContext& operator=(Http2ConnContext&&) = delete;

    auto getActiveStreams(
        size_t max_count = galay::kernel::UnsafeChannel<Http2Stream::ptr>::DEFAULT_BATCH_SIZE) {
        return GetActiveStreamsAwaitable(this, max_count);
    }

    bool isClosed() const {
        return m_closed;
    }

private:
    friend class GetActiveStreamsAwaitable;

    Http2ActiveStreamMailbox* m_mailbox;
    bool m_closed = false;
};

using Http2ActiveConnHandler = std::function<Task<void>(Http2ConnContext&)>;

/**
 * @brief HTTP/2 流管理器
 *
 * 职责：
 * 1. 运行帧读取协程（Reader），处理连接级帧，分发流级帧到对应 Http2Stream
 * 2. 运行帧写入协程（Writer），从连接内 send_queue 批量发送到 socket
 * 3. 新流创建后自动 spawn 用户 handler
 */
template<typename SocketType>
class Http2StreamManagerImpl
{
public:
    Http2StreamManagerImpl(Http2ConnImpl<SocketType>& conn)
        : m_conn(conn)
        , m_running(false)
    {
    }

    /**
     * @brief 启动流管理器（协程）
     * @param handler 用户流处理回调，每个新流创建后 spawn handler(stream)
     *
     * 内部启动两个协程：
     * - Reader: 读取帧、处理连接级帧、分发流级帧、spawn handler
     * - Writer: 从 send channel 接收数据并写入 socket
     */
    Task<void> start(Http2StreamHandler handler) {
        prepareForStart(false);
        if constexpr (is_ssl_socket_v<SocketType>) {
            if (!co_await startDetachedTask(monitorLoopThenNotify(&m_monitor_done))) {
                markDetachedStartupFailed(true, false);
                co_return;
            }
            m_writer_ready.notify();
            co_await sslServiceLoop(std::move(handler));
            co_await finishForegroundSslRun();
            co_return;
        }
        if (!co_await startBackgroundLoops()) {
            co_return;
        }
        co_await readerLoop(std::move(handler));
        co_await finishForegroundRun();
        co_return;
    }

    Task<void> start(Http2ActiveConnHandler handler) {
        prepareForStart(true);
        if constexpr (is_ssl_socket_v<SocketType>) {
            if (!co_await startDetachedTask(monitorLoopThenNotify(&m_monitor_done))) {
                markDetachedStartupFailed(true, false);
                co_return;
            }
            m_writer_ready.notify();

            Http2ConnContext ctx(m_active_stream_mailbox);
            m_active_handlers.fetch_add(1, std::memory_order_acq_rel);
            if (!co_await startDetachedTask(runActiveHandler(std::move(handler), &ctx))) {
                finishActiveHandlerSlot();
                closeActiveStreamQueue();
                m_conn.initiateClose();
                co_await finishForegroundSslRun();
                co_return;
            }

            co_await sslServiceLoop(nullptr);
            co_await finishForegroundSslRun();
            co_return;
        }

        if (!co_await startBackgroundLoops()) {
            co_return;
        }

        Http2ConnContext ctx(m_active_stream_mailbox);
        m_active_handlers.fetch_add(1, std::memory_order_acq_rel);
        if (!co_await startDetachedTask(runActiveHandler(std::move(handler), &ctx))) {
            finishActiveHandlerSlot();
            closeActiveStreamQueue();
            m_conn.initiateClose();
            co_await finishForegroundRun();
            co_return;
        }

        co_await readerLoop(nullptr);
        co_await finishForegroundRun();
        co_return;
    }

    /**
     * @brief 将帧入队发送
     */
    void enqueueSendFrame(Http2Frame::uptr frame,
                          const Http2OutgoingFrame::WaiterPtr& waiter = nullptr) {
        m_send_channel.send(Http2OutgoingFrame{std::move(frame), waiter});
    }

    void enqueueSendBytes(std::string bytes,
                          const Http2OutgoingFrame::WaiterPtr& waiter = nullptr) {
        m_send_channel.send(Http2OutgoingFrame{std::move(bytes), waiter});
    }

    template<typename FrameType>
    void enqueueSendFrame(FrameType&& frame,
                          const Http2OutgoingFrame::WaiterPtr& waiter = nullptr) {
        using FrameT = std::decay_t<FrameType>;
        static_assert(std::is_base_of_v<Http2Frame, FrameT>, "FrameType must derive from Http2Frame");
        m_send_channel.send(
            Http2OutgoingFrame{std::make_unique<FrameT>(std::forward<FrameType>(frame)), waiter});
    }

    bool isRunning() const { return m_running; }

    /**
     * @brief 获取连接引用（供用户 handler 使用）
     */
    Http2ConnImpl<SocketType>& conn() { return m_conn; }

    /**
     * @brief 从非协程上下文启动 StreamManager
     * @param scheduler 当前 IO 调度器
     * @param handler 用户流处理回调
     * @details 通过 scheduleTask(scheduler, ) 启动 reader/writer/monitor，
     *          不需要协程上下文，可从 CustomAwaitable::await_resume() 等普通函数调用。
     */
    bool startWithScheduler(galay::kernel::Scheduler* scheduler, Http2StreamHandler handler) {
        prepareForStart(false);
        if (scheduler == nullptr) {
            markDetachedStartupFailed(true, true);
            return false;
        }

        if constexpr (is_ssl_socket_v<SocketType>) {
            if (!scheduleTask(scheduler, monitorLoopThenNotify(&m_monitor_done))) {
                markDetachedStartupFailed(true, false);
                return false;
            }
            m_writer_ready.notify();
            if (!scheduleTask(scheduler, sslServiceLoopThenCleanup(std::move(handler)))) {
                m_running = false;
                closeActiveStreamQueue();
                m_stop_waiter.notify();
                return false;
            }
            return true;
        }

        bool writer_started = scheduleTask(scheduler, writerLoopThenNotify(&m_writer_done));
        bool monitor_started = scheduleTask(scheduler, monitorLoopThenNotify(&m_monitor_done));
        if (!writer_started || !monitor_started) {
            if (!writer_started) {
                m_writer_done.notify();
            }
            if (!monitor_started) {
                m_monitor_done.notify();
            }
            m_running = false;
            if (writer_started) {
                m_send_channel.send(Http2OutgoingFrame{});
            }
            m_writer_ready.notify();
            closeActiveStreamQueue();
            m_stop_waiter.notify();
            return false;
        }

        m_writer_ready.notify();
        if (!scheduleTask(scheduler, readerLoopThenCleanup(std::move(handler)))) {
            m_running = false;
            m_send_channel.send(Http2OutgoingFrame{});
            closeActiveStreamQueue();
            m_stop_waiter.notify();
            return false;
        }
        return true;
    }

    /**
     * @brief 自动分配 stream ID 并创建流
     * @details 客户端自动分配奇数 ID（3, 5, 7, ...），服务端自动分配偶数 ID（2, 4, 6, ...）
     */
    Http2Stream::ptr allocateStream() {
        uint32_t id = m_next_local_stream_id;
        m_next_local_stream_id += 2;
        return newStream(id);
    }

    /**
     * @brief 优雅关闭：发送 GOAWAY、关闭连接、等待 StreamManager 停止
     * @details 替代手动的 sendGoaway + conn.close() + waitStopped() 序列
     */
    Task<void> shutdown(Http2ErrorCode error = Http2ErrorCode::NoError) {
        return shutdownImpl(error);
    }

private:
    Task<void> shutdownImpl(Http2ErrorCode error = Http2ErrorCode::NoError) {
        if (!m_started) co_return;

        if (m_running) {
            m_conn.setDraining(true);

            if (m_conn.isClient()) {
                m_reject_new_streams = true;
                auto waiter = sendGoaway(error);
                if (waiter) {
                    co_await waiter->wait();
                }
            } else {
                // RFC 推荐的 graceful shutdown：先发 MAX_INT，再发真实 last_stream_id。
                auto first = sendGoaway(error, "draining", kMaxStreamId);
                if (first) {
                    co_await first->wait();
                }

                auto rtt = m_conn.runtimeConfig().graceful_shutdown_rtt;
                if (rtt.count() > 0) {
                    co_await galay::kernel::sleep(rtt);
                }

                m_reject_new_streams = true;
                auto last_accepted = m_conn.lastPeerStreamId();
                auto second = sendGoaway(error, "", last_accepted);
                if (second) {
                    co_await second->wait();
                }
            }

            // 服务端等待活跃流处理完成，避免直接断开造成业务中断。
            auto deadline = std::chrono::steady_clock::now() + m_conn.runtimeConfig().graceful_shutdown_timeout;
            while (m_active_handlers.load(std::memory_order_acquire) > 0 &&
                   std::chrono::steady_clock::now() < deadline) {
                co_await galay::kernel::sleep(std::chrono::milliseconds(5));
            }

            // 批量发送 RST_STREAM 给所有未完成的流
            std::vector<Http2OutgoingFrame> rst_frames;
            m_conn.forEachStream([&](uint32_t stream_id, Http2Stream::ptr& stream) {
                if (stream && stream->state() != Http2StreamState::Closed) {
                    auto bytes = Http2FrameBuilder::rstStreamBytes(stream_id, Http2ErrorCode::NoError);
                    stream->onRstStreamSent();
                    rst_frames.push_back(Http2OutgoingFrame{std::move(bytes), nullptr});
                }
            });

            // 批量入队 RST_STREAM 帧
            for (auto& frame : rst_frames) {
                m_send_channel.send(std::move(frame));
            }

            // 等待 RST_STREAM 帧发送完成
            if (!rst_frames.empty()) {
                co_await galay::kernel::sleep(std::chrono::milliseconds(10));
            }

            // 关闭所有流的帧队列
            m_conn.forEachStream([](uint32_t, Http2Stream::ptr& stream) {
                stream->closeFrameQueue();
            });

            // 先只触发 transport shutdown，保留现有 awaitable，
            // 让 readerLoop 从 readFramesBatch() 正常收到 closing/peer-closed 并退出。
            m_conn.initiateClose();
            if (m_running) {
                co_await waitStopped();
            }
            // readerLoop/Writer 全部退出后再真正 close fd，避免提前移除底层 READ/CUSTOM awaitable。
            co_await m_conn.close();
        }
        co_return;
    }

    void prepareForStart(bool active_conn_mode) {
        m_started = true;
        m_running = true;
        m_active_conn_mode = active_conn_mode;
        m_active_stream_queue_closed = false;
        m_active_stream_mailbox.reset();
        m_draining_handlers.store(false, std::memory_order_release);
        m_reject_new_streams = false;
        m_last_frame_recv_at = std::chrono::steady_clock::now();
        m_waiting_ping_ack = false;

        if (m_next_local_stream_id == 0) {
            m_next_local_stream_id = m_conn.isClient() ? 3 : 2;
        }
        if (!m_conn.isClient()) {
            const uint32_t target_window = m_conn.runtimeConfig().flow_control_target_window;
            const int32_t current_window = m_conn.connRecvWindow();
            if (target_window > 0 && static_cast<int32_t>(target_window) > current_window) {
                const auto increment = static_cast<uint32_t>(
                    static_cast<int64_t>(target_window) - current_window);
                enqueueWindowUpdateAction(0, increment);
                m_conn.adjustConnRecvWindow(static_cast<int32_t>(increment));
            }
        }
        m_conn.reserveStreams(
            static_cast<size_t>(std::max<uint32_t>(m_conn.localSettings().max_concurrent_streams, 64u)) + 8);
    }

    Task<bool> startBackgroundLoops() {
        bool writer_started = co_await startDetachedTask(writerLoopThenNotify(&m_writer_done));
        if (!writer_started) {
            markDetachedStartupFailed(true, true);
            co_return false;
        }

        bool monitor_started = co_await startDetachedTask(monitorLoopThenNotify(&m_monitor_done));
        if (!monitor_started) {
            m_running = false;
            m_send_channel.send(Http2OutgoingFrame{});
            m_monitor_done.notify();
            m_writer_ready.notify();
            closeActiveStreamQueue();
            m_stop_waiter.notify();
            co_return false;
        }

        m_writer_ready.notify();
        co_return true;
    }

    Task<void> finishForegroundRun() {
        m_draining_handlers.store(true, std::memory_order_release);
        if (m_active_handlers.load(std::memory_order_acquire) > 0) {
            co_await m_handler_waiter.wait();
        }

        m_running = false;
        m_send_channel.send(Http2OutgoingFrame{});
        co_await m_writer_done.wait();
        co_await m_monitor_done.wait();
        m_stop_waiter.notify();
        co_return;
    }

    Task<void> finishForegroundSslRun() {
        m_draining_handlers.store(true, std::memory_order_release);
        if (m_active_handlers.load(std::memory_order_acquire) > 0) {
            co_await m_handler_waiter.wait();
        }

        m_running = false;
        co_await m_monitor_done.wait();
        m_stop_waiter.notify();
        co_return;
    }

    Task<void> readerLoopThenCleanup(Http2StreamHandler handler) {
        co_await readerLoop(std::move(handler));

        m_draining_handlers.store(true, std::memory_order_release);
        if (m_active_handlers.load(std::memory_order_acquire) > 0) {
            co_await m_handler_waiter.wait();
        }

        m_running = false;
        m_send_channel.send(Http2OutgoingFrame{});
        co_await m_writer_done.wait();
        co_await m_monitor_done.wait();
        m_stop_waiter.notify();
        co_return;
    }

    Task<void> sslServiceLoopThenCleanup(Http2StreamHandler handler) {
        co_await sslServiceLoop(std::move(handler));

        m_draining_handlers.store(true, std::memory_order_release);
        if (m_active_handlers.load(std::memory_order_acquire) > 0) {
            co_await m_handler_waiter.wait();
        }

        m_running = false;
        co_await m_monitor_done.wait();
        m_stop_waiter.notify();
        co_return;
    }

    void markDetachedStartupFailed(bool monitor_done, bool writer_done) {
        m_running = false;
        if (monitor_done) {
            m_monitor_done.notify();
        }
        if (writer_done) {
            m_writer_done.notify();
        }
        m_writer_ready.notify();
        closeActiveStreamQueue();
        m_stop_waiter.notify();
    }

    void finishActiveHandlerSlot() {
        int remaining = m_active_handlers.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (remaining == 0 && m_draining_handlers.load(std::memory_order_acquire)) {
            m_handler_waiter.notify();
        }
    }

public:
    /**
     * @brief 发送 GOAWAY 帧
     * @return waiter，co_await waiter->wait() 等待发送完成
     */
    Http2OutgoingFrame::WaiterPtr sendGoaway(Http2ErrorCode error = Http2ErrorCode::NoError,
                                             const std::string& debug = "",
                                             std::optional<uint32_t> last_stream_id = std::nullopt) {
        auto waiter = std::make_shared<Http2OutgoingFrame::Waiter>();
        enqueueGoaway(error, debug, waiter, last_stream_id);
        return waiter;
    }

    /**
     * @brief 等待 StreamManager 停止（start() 完成）
     */
    galay::kernel::AsyncWaiterAwaitable<void> waitStopped() {
        return m_stop_waiter.wait();
    }

private:
    static constexpr size_t kSslIoOwnerBatchSize = 64;
    static constexpr auto kSslIoOwnerHotWaitInterval = std::chrono::milliseconds(1);
    static constexpr auto kSslIoOwnerActivePollInterval = std::chrono::milliseconds(5);
    static constexpr auto kSslIoOwnerIdlePollInterval = std::chrono::milliseconds(50);

    void collectOutgoingFrame(Http2OutgoingFrame&& item,
                              std::vector<Http2OutgoingFrame>& outgoing_batch,
                              std::vector<Http2OutgoingFrame::WaiterPtr>& waiters,
                              bool& has_shutdown) {
        if (item.isEmpty()) {
            has_shutdown = true;
            return;
        }
        if (item.waiter) {
            waiters.push_back(item.waiter);
        }
        outgoing_batch.push_back(std::move(item));
    }

    void drainOutgoingChannel(std::vector<Http2OutgoingFrame>& outgoing_batch,
                              std::vector<Http2OutgoingFrame::WaiterPtr>& waiters,
                              bool& has_shutdown) {
        while (!has_shutdown) {
            auto item = m_send_channel.tryRecv();
            if (!item) {
                break;
            }
            collectOutgoingFrame(std::move(*item), outgoing_batch, waiters, has_shutdown);
        }
    }

    void notifyWaiters(std::vector<Http2OutgoingFrame::WaiterPtr>& waiters) {
        for (auto& waiter : waiters) {
            if (waiter) {
                waiter->notify();
            }
        }
    }

    bool shouldHotWaitForOutgoing(bool had_ready_active_streams,
                                  bool had_pending_spawns) const {
        if (had_ready_active_streams || had_pending_spawns) {
            return true;
        }
        if (m_active_conn_mode) {
            return false;
        }
        return m_active_handlers.load(std::memory_order_acquire) > 0;
    }

    std::chrono::milliseconds sslIoOwnerPollInterval(bool low_latency_mode) const {
        return low_latency_mode ? kSslIoOwnerActivePollInterval : kSslIoOwnerIdlePollInterval;
    }

    bool shouldEnforceSettingsAckTimeout(std::chrono::steady_clock::time_point now) const {
        const auto settings_timeout = m_conn.runtimeConfig().settings_ack_timeout;
        if (settings_timeout.count() <= 0 || !m_conn.isSettingsAckPending()) {
            return false;
        }
        if (now - m_conn.settingsSentAt() <= settings_timeout) {
            return false;
        }

        // Once the peer is actively sending frames after our SETTINGS, prefer to
        // keep the connection alive and validate behavior via actual frame-level
        // protocol checks instead of timing out a missing ACK.
        return m_last_frame_recv_at <= m_conn.settingsSentAt();
    }

    Task<bool> sendSslOutgoingBatch(const std::vector<Http2OutgoingFrame>& outgoing_batch,
                                    std::vector<Http2OutgoingFrame::WaiterPtr>& waiters,
                                    std::string& coalesced_buffer) {
        if (outgoing_batch.empty()) {
            co_return true;
        }

        size_t total_bytes = 0;
        for (const auto& item : outgoing_batch) {
            total_bytes += item.serializedSize();
        }

        coalesced_buffer.clear();
        if (coalesced_buffer.capacity() < total_bytes) {
            coalesced_buffer.reserve(total_bytes);
        }
        for (const auto& item : outgoing_batch) {
            item.appendTo(coalesced_buffer);
        }

        size_t offset = 0;
        while (offset < coalesced_buffer.size()) {
            auto send_result = co_await m_conn.socket().send(
                coalesced_buffer.data() + offset,
                coalesced_buffer.size() - offset);
            if (!send_result || send_result.value() == 0) {
                if (m_conn.isClosing() || m_conn.isPeerClosed() ||
                    m_conn.isGoawaySent() || m_conn.isGoawayReceived()) {
                } else if (send_result) {
                } else {
                }
                notifyWaiters(waiters);
                m_conn.forEachStream([](uint32_t, Http2Stream::ptr& stream) {
                    stream->closeFrameQueue();
                });
                closeActiveStreamQueue();
                co_return false;
            }
            offset += send_result.value();
        }

        co_return true;
    }

    Task<void> writerLoopThenNotify(AsyncWaiter<void>* done) {
        try {
            co_await writerLoop();
        } catch (...) {
            if (done) {
                done->notify();
            }
            closeActiveStreamQueue();
            co_return;
        }
        if (done) {
            done->notify();
        }
        co_return;
    }

    Task<void> monitorLoopThenNotify(AsyncWaiter<void>* done) {
        try {
            co_await monitorLoop();
        } catch (...) {
            if (done) {
                done->notify();
            }
            closeActiveStreamQueue();
            co_return;
        }
        if (done) {
            done->notify();
        }
        co_return;
    }

    Http2Stream::ptr newStream(uint32_t stream_id) {
        auto stream = m_conn.getStream(stream_id);
        if (stream) {
            attachStreamIO(stream);
            return stream;
        }
        return createStreamInternal(stream_id);
    }

    /**
     * @brief Reader 协程：读取帧、处理连接级帧、分发流级帧
     */
    Task<void> readerLoop(Http2StreamHandler handler) {
        // readerLoop 只在 IO 错误（peer closed / connection error）或连接关闭时退出。
        // GOAWAY（无论收到还是发出）不退出：GOAWAY 只表示不再有新流，
        // 已有流的帧仍需继续读取直到连接关闭（RFC 9113 §6.8）。
        while (true) {
            if constexpr (!is_ssl_socket_v<SocketType>) {
                if (m_active_conn_mode && !m_conn.isClient()) {
                    auto frame_views_result = co_await m_conn.readFrameViewsBatch();

                    if (!frame_views_result) {
                        if (m_conn.isClosing() || m_conn.isPeerClosed()) {
                            break;
                        }
                        if (frame_views_result.error() == Http2ErrorCode::NoError) {
                            continue;
                        }
                        if (frame_views_result.error() == Http2ErrorCode::ProtocolError &&
                            (m_conn.isPeerClosed() || m_conn.isClosing())) {
                            break;
                        }
                        if (m_conn.lastReadError().empty()) {
                        } else {
                        }
                        enqueueGoaway(frame_views_result.error());
                        break;
                    }

                    bool exit_loop = false;
                    auto& frame_views = *frame_views_result;
                    for (auto& frame_view : frame_views) {
                        const uint32_t stream_id = frame_view.streamId();
                        m_last_frame_recv_at = std::chrono::steady_clock::now();


                        if (m_conn.isExpectingContinuation()) {
                            if (!frame_view.isContinuation() ||
                                stream_id != m_conn.continuationStreamId()) {
                                enqueueGoaway(Http2ErrorCode::ProtocolError);
                                exit_loop = true;
                                break;
                            }
                        }

                        if (frame_view.isConnectionFrame()) {
                            auto frame = materializeFrameView(frame_view);
                            if (!frame) {
                                enqueueGoaway(frame.error());
                                exit_loop = true;
                                break;
                            }
                            handleConnectionFrame(std::move(*frame));
                            continue;
                        }

                        if (tryDispatchServerActiveFrameView(std::move(frame_view))) {
                            continue;
                        }

                        auto frame = materializeFrameView(frame_view);
                        if (!frame) {
                            enqueueGoaway(frame.error());
                            exit_loop = true;
                            break;
                        }
                        dispatchStreamFrame(std::move(*frame));
                    }

                    processPendingActions();
                    drainRetiredStreams();
                    flushActiveStreams();

                    while (!m_pending_spawns.empty()) {
                        auto stream = m_pending_spawns.top();
                        m_pending_spawns.pop();
                        m_active_handlers.fetch_add(1, std::memory_order_acq_rel);
                        if (!co_await startDetachedTask(runHandler(handler, stream))) {
                            if (stream) {
                                stream->closeFrameQueue();
                                enqueueRetireStream(stream->streamId());
                            }
                            finishActiveHandlerSlot();
                        }
                    }

                    if (exit_loop) {
                        break;
                    }
                    continue;
                }
            }

            auto frames_result = co_await m_conn.readFramesBatch();

            if (!frames_result) {
                if (m_conn.isClosing() || m_conn.isPeerClosed()) {
                    break;
                }
                if (frames_result.error() == Http2ErrorCode::NoError) {
                    continue;
                }
                if (frames_result.error() == Http2ErrorCode::ProtocolError &&
                    (m_conn.isPeerClosed() || m_conn.isClosing())) {
                    break;
                }
                if (m_conn.lastReadError().empty()) {
                } else {
                }
                enqueueGoaway(frames_result.error());
                break;
            }

            bool exit_loop = false;
            auto& frames = *frames_result;
            for (auto& frame : frames) {
                uint32_t stream_id = frame->streamId();
                m_last_frame_recv_at = std::chrono::steady_clock::now();


                // CONTINUATION 状态检查
                if (m_conn.isExpectingContinuation()) {
                    if (!frame->isContinuation() || stream_id != m_conn.continuationStreamId()) {
                        enqueueGoaway(Http2ErrorCode::ProtocolError);
                        exit_loop = true;
                        break;
                    }
                }

                // 连接级帧
                if (frame->isSettings() || frame->isPing() || frame->isGoAway() ||
                    (frame->isWindowUpdate() && stream_id == 0)) {
                    handleConnectionFrame(std::move(frame));
                    continue;
                }

                // 流级帧 → 分发到 Http2Stream 帧队列
                dispatchStreamFrame(std::move(frame));
            }

            // 处理 dispatchStreamFrame 中标记的待处理动作
            processPendingActions();
            flushActiveStreams();

            // spawn 待处理的流 handler
            while (!m_pending_spawns.empty()) {
                auto stream = m_pending_spawns.top();
                m_pending_spawns.pop();
                m_active_handlers.fetch_add(1, std::memory_order_acq_rel);
                if (!co_await startDetachedTask(runHandler(handler, stream))) {
                    if (stream) {
                        stream->closeFrameQueue();
                        enqueueRetireStream(stream->streamId());
                    }
                    finishActiveHandlerSlot();
                }
            }

            if (exit_loop) {
                break;
            }
        }
        // 关闭所有流的帧队列
        m_conn.forEachStream([](uint32_t, Http2Stream::ptr& stream) {
            stream->closeFrameQueue();
        });
        closeActiveStreamQueue();
        drainRetiredStreams();

        co_return;
    }

    Task<void> sslServiceLoop(Http2StreamHandler handler) {
        std::vector<Http2OutgoingFrame> outgoing_batch;
        std::vector<Http2OutgoingFrame::WaiterPtr> waiters;
        std::vector<uint8_t> frame_scratch;
        std::string coalesced_buffer;
        std::vector<char> recv_scratch;
        outgoing_batch.reserve(64);
        waiters.reserve(64);
        frame_scratch.reserve(65536);
        coalesced_buffer.reserve(65536);
        recv_scratch.reserve(65536);

        while (true) {
            outgoing_batch.clear();
            waiters.clear();

            bool has_shutdown = false;
            drainOutgoingChannel(outgoing_batch, waiters, has_shutdown);

            if (!outgoing_batch.empty()) {
                if (!co_await sendSslOutgoingBatch(outgoing_batch, waiters, coalesced_buffer)) {
                    co_return;
                }
            }
            notifyWaiters(waiters);

            bool exit_loop = false;
            bool had_ready_active_streams = false;
            bool had_pending_spawns = false;
            while (!exit_loop) {
                if (m_active_conn_mode && !m_conn.isClient()) {
                    auto frame_views_result = detail::parseBufferedFrameViewBatch(
                        m_conn.ringBuffer(),
                        m_conn.peerSettings().max_frame_size,
                        std::numeric_limits<size_t>::max());
                    if (!frame_views_result) {
                        if (m_conn.isClosing() || m_conn.isPeerClosed()) {
                            exit_loop = true;
                            break;
                        }
                        if (frame_views_result.error() == Http2ErrorCode::NoError) {
                            break;
                        }
                        if (m_conn.lastReadError().empty()) {
                        } else {
                        }
                        enqueueGoaway(frame_views_result.error());
                        exit_loop = true;
                        break;
                    }
                    if (frame_views_result->empty()) {
                        break;
                    }

                    auto& frame_views = *frame_views_result;
                    for (auto& frame_view : frame_views) {
                        const uint32_t stream_id = frame_view.streamId();
                        m_last_frame_recv_at = std::chrono::steady_clock::now();


                        if (m_conn.isExpectingContinuation()) {
                            if (!frame_view.isContinuation() ||
                                stream_id != m_conn.continuationStreamId()) {
                                enqueueGoaway(Http2ErrorCode::ProtocolError);
                                exit_loop = true;
                                break;
                            }
                        }

                        if (frame_view.isConnectionFrame()) {
                            auto frame = materializeFrameView(frame_view);
                            if (!frame) {
                                enqueueGoaway(frame.error());
                                exit_loop = true;
                                break;
                            }
                            handleConnectionFrame(std::move(*frame));
                            continue;
                        }

                        if (tryDispatchServerActiveFrameView(std::move(frame_view))) {
                            continue;
                        }

                        auto frame = materializeFrameView(frame_view);
                        if (!frame) {
                            enqueueGoaway(frame.error());
                            exit_loop = true;
                            break;
                        }
                        dispatchStreamFrame(std::move(*frame));
                    }

                    had_ready_active_streams = had_ready_active_streams || !m_active_batch.empty();
                    processPendingActions();
                    flushActiveStreams();
                    if (exit_loop) {
                        break;
                    }
                    continue;
                }

                auto frames_result = detail::parseBufferedFrameBatch(
                    m_conn.ringBuffer(),
                    m_conn.peerSettings().max_frame_size,
                    std::numeric_limits<size_t>::max(),
                    frame_scratch);
                if (!frames_result) {
                    if (m_conn.isClosing() || m_conn.isPeerClosed()) {
                        exit_loop = true;
                        break;
                    }
                    if (frames_result.error() == Http2ErrorCode::NoError) {
                        break;
                    }
                    if (m_conn.lastReadError().empty()) {
                    } else {
                    }
                    enqueueGoaway(frames_result.error());
                    exit_loop = true;
                    break;
                }
                if (frames_result->empty()) {
                    break;
                }

                auto& frames = *frames_result;
                for (auto& frame : frames) {
                    uint32_t stream_id = frame->streamId();
                    m_last_frame_recv_at = std::chrono::steady_clock::now();


                    if (m_conn.isExpectingContinuation()) {
                        if (!frame->isContinuation() || stream_id != m_conn.continuationStreamId()) {
                            enqueueGoaway(Http2ErrorCode::ProtocolError);
                            exit_loop = true;
                            break;
                        }
                    }

                    if (frame->isSettings() || frame->isPing() || frame->isGoAway() ||
                        (frame->isWindowUpdate() && stream_id == 0)) {
                        handleConnectionFrame(std::move(frame));
                        continue;
                    }

                    dispatchStreamFrame(std::move(frame));
                }

                had_ready_active_streams =
                    had_ready_active_streams || (m_active_conn_mode && !m_active_batch.empty());
                processPendingActions();
                drainRetiredStreams();
                flushActiveStreams();

                had_pending_spawns = had_pending_spawns || !m_pending_spawns.empty();
                while (!m_pending_spawns.empty()) {
                    auto stream = m_pending_spawns.top();
                    m_pending_spawns.pop();
                    m_active_handlers.fetch_add(1, std::memory_order_acq_rel);
                    if (!co_await startDetachedTask(runHandler(handler, stream))) {
                        if (stream) {
                            stream->closeFrameQueue();
                            enqueueRetireStream(stream->streamId());
                        }
                        finishActiveHandlerSlot();
                    }
                }
            }

            if (exit_loop || has_shutdown) {
                break;
            }

            if (!m_send_channel.empty()) {
                drainRetiredStreams();
                continue;
            }

            const bool hot_wait_outgoing =
                shouldHotWaitForOutgoing(had_ready_active_streams, had_pending_spawns);
            if (hot_wait_outgoing) {
                auto hot_batch_result = co_await m_send_channel.recvBatch(kSslIoOwnerBatchSize)
                    .timeout(kSslIoOwnerHotWaitInterval);
                if (hot_batch_result) {
                    outgoing_batch.clear();
                    waiters.clear();
                    has_shutdown = false;

                    for (auto& item : *hot_batch_result) {
                        collectOutgoingFrame(std::move(item), outgoing_batch, waiters, has_shutdown);
                        if (has_shutdown) {
                            break;
                        }
                    }
                    drainOutgoingChannel(outgoing_batch, waiters, has_shutdown);

                    if (!outgoing_batch.empty()) {
                        if (!co_await sendSslOutgoingBatch(outgoing_batch, waiters, coalesced_buffer)) {
                            co_return;
                        }
                    }
                    notifyWaiters(waiters);
                    if (has_shutdown) {
                        break;
                    }
                    drainRetiredStreams();
                    continue;
                }
                if (hot_batch_result.error().code() != kTimeout) {
                    break;
                }
                if (!m_send_channel.empty()) {
                    continue;
                }
            }

            char* recv_buffer = nullptr;
            size_t recv_length = 0;
            auto write_iovecs = borrowWriteIovecs(m_conn.ringBuffer());
            const struct iovec* first_write_iov = IoVecWindow::firstNonEmpty(write_iovecs);
            if (first_write_iov == nullptr) {
                m_conn.setLastReadError("RingBuffer is full");
                enqueueGoaway(Http2ErrorCode::ProtocolError);
                break;
            }
            size_t total_writable = 0;
            for (const auto& iov : write_iovecs) {
                total_writable += iov.iov_len;
            }
            const bool staged_recv = first_write_iov->iov_len != total_writable;
            if (staged_recv) {
                recv_scratch.resize(total_writable);
                recv_buffer = recv_scratch.data();
                recv_length = recv_scratch.size();
            } else {
                recv_buffer = static_cast<char*>(first_write_iov->iov_base);
                recv_length = first_write_iov->iov_len;
            }
            if (recv_length == 0) {
                m_conn.setLastReadError("RingBuffer is full");
                enqueueGoaway(Http2ErrorCode::ProtocolError);
                break;
            }

            bool has_ssl_pending_plaintext = false;
            if constexpr (requires(SocketType& socket) { socket.engine(); }) {
                auto* engine = m_conn.socket().engine();
                has_ssl_pending_plaintext = engine != nullptr && engine->pending() > 0;
            }

            auto recv_result = has_ssl_pending_plaintext
                ? (co_await m_conn.socket().recv(recv_buffer, recv_length))
                : (co_await m_conn.socket().recv(recv_buffer, recv_length)
                    .timeout(sslIoOwnerPollInterval(hot_wait_outgoing)));
            if (!recv_result) {
                const auto& error = recv_result.error();
#ifdef GALAY_SSL_FEATURE_ENABLED
                if (error.code() == galay::ssl::SslErrorCode::kTimeout) {
                    continue;
                }
                if (error.code() == galay::ssl::SslErrorCode::kPeerClosed) {
                    m_conn.markPeerClosed(error.message());
                    break;
                }
#endif
                if (m_conn.isClosing()) {
                    m_conn.setLastReadError(error.message());
                    break;
                }
                m_conn.setLastReadError(error.message());
                enqueueGoaway(Http2ErrorCode::ProtocolError);
                break;
            }

            const size_t bytes_read = recv_result->size();
            if (bytes_read == 0) {
                m_conn.markPeerClosed();
                break;
            }

            m_conn.clearLastReadError();
            if (staged_recv) {
                m_conn.feedData(recv_scratch.data(), bytes_read);
            } else {
                m_conn.ringBuffer().produce(bytes_read);
            }
        }

        m_conn.forEachStream([](uint32_t, Http2Stream::ptr& stream) {
            stream->closeFrameQueue();
        });
        closeActiveStreamQueue();
        drainRetiredStreams();
        co_return;
    }

    Task<void> runHandler(Http2StreamHandler handler, Http2Stream::ptr stream) {
        co_await handler(stream);
        // Handler 可能在非 IO owner 线程恢复，流表回收统一回送给主循环串行处理。
        enqueueRetireStream(stream ? stream->streamId() : 0);
        int remaining = m_active_handlers.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (remaining == 0 && m_draining_handlers.load(std::memory_order_acquire)) {
            m_handler_waiter.notify();
        }
        co_return;
    }

    Task<void> runActiveHandler(Http2ActiveConnHandler handler, Http2ConnContext* ctx) {
        co_await handler(*ctx);
        if (m_running && !m_conn.isClosing()) {
            m_conn.initiateClose();
        }
        int remaining = m_active_handlers.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (remaining == 0 && m_draining_handlers.load(std::memory_order_acquire)) {
            m_handler_waiter.notify();
        }
        co_return;
    }

    Task<void> monitorLoop() {
        while (m_running) {
            co_await galay::kernel::sleep(std::chrono::milliseconds(100));
            if (!m_running) {
                break;
            }

            auto now = std::chrono::steady_clock::now();

            if (shouldEnforceSettingsAckTimeout(now)) {
                enqueueGoaway(Http2ErrorCode::SettingsTimeout, "SETTINGS ACK timeout");
                m_conn.initiateClose();
                break;
            }

            if (!m_conn.runtimeConfig().ping_enabled ||
                m_conn.runtimeConfig().ping_interval.count() <= 0) {
                continue;
            }

            if (!m_waiting_ping_ack) {
                if (now - m_last_frame_recv_at >= m_conn.runtimeConfig().ping_interval) {
                    Http2PingFrame ping;
                    m_last_ping_payload.fill(0);
                    auto nonce = static_cast<uint64_t>(
                        std::chrono::duration_cast<std::chrono::microseconds>(
                            now.time_since_epoch()).count());
                    for (int i = 0; i < 8; ++i) {
                        m_last_ping_payload[7 - i] = static_cast<uint8_t>((nonce >> (i * 8)) & 0xFF);
                    }
                    ping.setOpaqueData(m_last_ping_payload.data());
                    enqueueSendFrame(std::move(ping));
                    m_waiting_ping_ack = true;
                    m_last_ping_sent_at = now;
                }
            } else if (m_conn.runtimeConfig().ping_timeout.count() > 0 &&
                       now - m_last_ping_sent_at > m_conn.runtimeConfig().ping_timeout) {
                enqueueGoaway(Http2ErrorCode::ProtocolError, "PING ACK timeout");
                m_conn.initiateClose();
                break;
            }
        }
        co_return;
    }

    /**
     * @brief Writer 协程：从 send channel 接收数据并写入 socket
     * @details 使用 writev 批量发送多个帧，减少系统调用和内存拷贝
     */
    Task<void> writerLoop() {
        // 预分配待发送包和 iovec 数组，避免每次循环分配
        std::vector<Http2OutgoingFrame> outgoing_batch;
        std::vector<std::string> flattened_buffers;
        IoVecWriteState write_state;
        std::vector<Http2OutgoingFrame::WaiterPtr> waiters;
        outgoing_batch.reserve(64);
        flattened_buffers.reserve(64);
        write_state.reserve(64);
        waiters.reserve(64);

        while (true) {
            auto item_result = co_await m_send_channel.recv();
            if (!item_result) {
                break;
            }

            outgoing_batch.clear();
            flattened_buffers.clear();
            write_state.clear();
            waiters.clear();

            bool has_shutdown = false;
            auto collect_item = [&](Http2OutgoingFrame&& item) {
                if (item.isEmpty()) {
                    // 收到关闭信号，先发送已有数据再退出
                    has_shutdown = true;
                    return;
                }
                if (item.waiter) {
                    waiters.push_back(std::move(item.waiter));
                }
                outgoing_batch.push_back(std::move(item));
            };

            collect_item(std::move(item_result.value()));

            while (!has_shutdown) {
                auto next = m_send_channel.tryRecv();
                if (!next.has_value()) {
                    break;
                }
                collect_item(std::move(next.value()));
            }

            if (!outgoing_batch.empty()) {
                flattened_buffers.reserve(outgoing_batch.size());
                write_state.reserve(outgoing_batch.size() * 2);
                for (auto& item : outgoing_batch) {
                    if (item.frame) {
                        flattened_buffers.push_back(item.frame->serialize());
                        auto& buffer = flattened_buffers.back();
                        write_state.append({
                            .iov_base = buffer.data(),
                            .iov_len = buffer.size()
                        });
                        continue;
                    }

                    std::array<struct iovec, 2> iovecs{};
                    const size_t count = item.exportIovecs(iovecs);
                    if (count == 0) {
                        continue;
                    }
                    for (size_t i = 0; i < count; ++i) {
                        write_state.append(iovecs[i]);
                    }
                }
            }

            if (!write_state.empty()) {
                if constexpr (requires(SocketType& socket, std::vector<iovec>& vec) { socket.writev(vec); }) {
                    // 支持 writev 的 socket（如 TcpSocket）：一次批量发送
                    while (!write_state.empty()) {
                        auto result = co_await m_conn.socket().writev(
                            std::span<const struct iovec>(write_state.data(), write_state.count()));
                        if (!result) {
                            if (m_conn.isClosing() || m_conn.isPeerClosed() ||
                                m_conn.isGoawaySent() || m_conn.isGoawayReceived()) {
                            } else {
                            }
                            for (auto& waiter : waiters) {
                                if (waiter) {
                                    waiter->notify();
                                }
                            }
                            co_return;
                        }
                        const size_t written = result.value();
                        if (written == 0) {
                            for (auto& waiter : waiters) {
                                if (waiter) {
                                    waiter->notify();
                                }
                            }
                            co_return;
                        }

                        if (write_state.advance(written) != written) {
                            for (auto& waiter : waiters) {
                                if (waiter) {
                                    waiter->notify();
                                }
                            }
                            co_return;
                        }
                }
            } else {
                    // 不支持 writev 的 socket（如 SslSocket）：按包串行 flatten 后 send
                    flattened_buffers.clear();
                    flattened_buffers.reserve(outgoing_batch.size());
                    for (const auto& item : outgoing_batch) {
                        flattened_buffers.push_back(item.flatten());
                    }
                    for (const auto& buffer : flattened_buffers) {
                        size_t offset = 0;
                        while (offset < buffer.size()) {
                            auto result = co_await m_conn.socket().send(buffer.data() + offset, buffer.size() - offset);
                            if (!result || result.value() == 0) {
                                if (m_conn.isClosing() || m_conn.isPeerClosed() ||
                                    m_conn.isGoawaySent() || m_conn.isGoawayReceived()) {
                                } else {
                                }
                                for (auto& waiter : waiters) {
                                    if (waiter) {
                                        waiter->notify();
                                    }
                                }
                                co_return;
                            }
                            offset += result.value();
                        }
                    }
                }
            }

            // 通知所有 waiter
            for (auto& waiter : waiters) {
                if (waiter) {
                    waiter->notify();
                }
            }

            if (has_shutdown) {
                co_return;
            }
        }

        co_return;
    }

    /**
     * @brief 处理连接级帧（非协程，通过 channel 发送响应）
     */
    void handleConnectionFrame(Http2Frame::uptr frame) {
        switch (frame->type()) {
            case Http2FrameType::Settings: {
                auto* settings = frame->asSettings();
                if (settings->isAck()) {
                    m_conn.markSettingsAckReceived();
                } else {
                    auto err = m_conn.peerSettings().applySettings(*settings);
                    if (err != Http2ErrorCode::NoError) {
                        enqueueGoaway(err);
                        return;
                    }
                    m_conn.encoder().setMaxTableSize(m_conn.peerSettings().header_table_size);

                    Http2SettingsFrame ack;
                    ack.setAck(true);
                    enqueueSendFrame(std::move(ack));
                }
                break;
            }

            case Http2FrameType::Ping: {
                auto* ping = frame->asPing();
                if (frame->streamId() != 0) {
                    enqueueGoaway(Http2ErrorCode::ProtocolError);
                    return;
                }
                if (!ping->isAck()) {
                    Http2PingFrame pong;
                    pong.setOpaqueData(ping->opaqueData());
                    pong.setAck(true);
                    enqueueSendFrame(std::move(pong));
                } else if (m_waiting_ping_ack &&
                           std::memcmp(ping->opaqueData(), m_last_ping_payload.data(), 8) == 0) {
                    m_waiting_ping_ack = false;
                }
                break;
            }

            case Http2FrameType::GoAway: {
                auto* goaway = frame->asGoAway();
                m_reject_new_streams = true;
                m_conn.markGoawayReceived(
                    goaway->lastStreamId(), goaway->errorCode(), goaway->debugData());

                if (m_conn.isClient()) {
                    const uint32_t last = goaway->lastStreamId();
                    m_conn.forEachStream([&](uint32_t stream_id, Http2Stream::ptr& stream) {
                        if (!stream || stream_id <= last) {
                            return;
                        }
                        Http2GoAwayError err;
                        err.stream_id = stream_id;
                        err.last_stream_id = last;
                        err.error_code = goaway->errorCode();
                        err.retryable = true;
                        err.debug = goaway->debugData();
                        stream->setGoAwayError(std::move(err));
                        stream->closeFrameQueue();
                    });
                }
                break;
            }

            case Http2FrameType::WindowUpdate: {
                auto* wu = frame->asWindowUpdate();
                uint32_t increment = wu->windowSizeIncrement();
                if (increment == 0) {
                    enqueueGoaway(Http2ErrorCode::ProtocolError);
                    return;
                }
                m_conn.adjustConnSendWindow(increment);
                break;
            }

            default:
                break;
        }
    }

    /**
     * @brief 分发流级帧到对应 Http2Stream 的帧队列
     */
    void enqueueGoawayAction(Http2ErrorCode error) {
        m_pending_actions.push_back({PendingAction::Type::SendGoaway, 0, error});
    }

    void enqueueRstStreamAction(uint32_t stream_id, Http2ErrorCode error) {
        m_pending_actions.push_back({PendingAction::Type::SendRstStream, stream_id, error});
    }

    void enqueueWindowUpdateAction(uint32_t stream_id, uint32_t increment) {
        m_pending_actions.push_back({
            PendingAction::Type::SendWindowUpdate, stream_id, Http2ErrorCode::NoError, increment});
    }

    void enqueueRetireStream(uint32_t stream_id) {
        if (stream_id == 0) {
            return;
        }
        m_retire_stream_channel.send(stream_id);
    }

    void drainRetiredStreams() {
        while (auto stream_id = m_retire_stream_channel.tryRecv()) {
            clearHotStream(*stream_id);
            m_conn.removeStream(*stream_id);
        }
    }

    Http2Stream::ptr findAttachedStream(uint32_t stream_id) {
        if (m_hot_stream && m_hot_stream->streamId() == stream_id) {
            return m_hot_stream;
        }
        auto stream = m_conn.getStream(stream_id);
        attachStreamIO(stream);
        rememberHotStream(stream);
        return stream;
    }

    Http2Stream::ptr findOrCreateHeadersStream(uint32_t stream_id) {
        auto stream = findAttachedStream(stream_id);
        if (stream) {
            return stream;
        }

        if (m_conn.isClient()) {
            enqueueGoawayAction(Http2ErrorCode::ProtocolError);
            return nullptr;
        }

        if (m_reject_new_streams ||
            (m_conn.isGoawaySent() && m_conn.goawayLastStreamId() != kMaxStreamId)) {
            enqueueRstStreamAction(stream_id, Http2ErrorCode::RefusedStream);
            return nullptr;
        }
        if (stream_id <= m_conn.lastPeerStreamId()) {
            enqueueGoawayAction(Http2ErrorCode::ProtocolError);
            return nullptr;
        }
        if (m_conn.streamCount() >= m_conn.localSettings().max_concurrent_streams) {
            enqueueRstStreamAction(stream_id, Http2ErrorCode::RefusedStream);
            return nullptr;
        }

        stream = createStreamInternal(stream_id);
        m_conn.setLastPeerStreamId(stream_id);
        return stream;
    }

    void decodeBufferedHeaders(const Http2Stream::ptr& stream) {
        auto fields = m_conn.decoder().decode(stream->headerBlock());
        if (fields) {
            stream->setDecodedHeaders(std::move(fields.value()));
        }
        stream->clearHeaderBlock();
        m_conn.setExpectingContinuation(false);
    }

    void completeReceivedHeaders(const Http2Stream::ptr& stream, bool end_stream) {
        decodeBufferedHeaders(stream);

        if (m_conn.isClient()) {
            stream->consumeDecodedHeadersAsResponse();
            auto events = Http2StreamEvent::HeadersReady;
            if (end_stream) {
                stream->markResponseCompleted();
                events |= Http2StreamEvent::ResponseComplete;
            }
            markStreamActive(stream, events);
            return;
        }

        stream->consumeDecodedHeadersAsRequest();
        auto events = Http2StreamEvent::HeadersReady;
        if (end_stream) {
            stream->markRequestCompleted();
            events |= Http2StreamEvent::RequestComplete;
        }
        if (m_active_conn_mode) {
            if (shouldDeferHeadersOnlyActiveDelivery(stream, end_stream)) {
                stream->m_pending_events |= events;
                return;
            }
            markStreamActive(stream, events);
        } else {
            queueStreamHandler(stream);
        }
    }

    void applyRecvWindowUpdate(const Http2Stream::ptr& stream, uint32_t stream_id, size_t data_size) {
        auto update = m_conn.evaluateRecvWindowUpdate(stream->recvWindow(), data_size);
        if (update.conn_increment > 0) {
            enqueueWindowUpdateAction(0, update.conn_increment);
            m_conn.adjustConnRecvWindow(static_cast<int32_t>(update.conn_increment));
        }
        if (update.stream_increment > 0) {
            enqueueWindowUpdateAction(stream_id, update.stream_increment);
            stream->adjustRecvWindow(static_cast<int32_t>(update.stream_increment));
        }
    }

    void appendStreamDataAndMarkEvents(const Http2Stream::ptr& stream, Http2DataFrame* data) {
        auto events = Http2StreamEvent::DataArrived;
        if (m_conn.isClient()) {
            stream->appendResponseData(data->data());
            if (data->isEndStream()) {
                stream->markResponseCompleted();
                events |= Http2StreamEvent::ResponseComplete;
            }
        } else {
            if (m_active_conn_mode) {
                stream->appendRequestData(std::move(data->data()));
            } else {
                stream->appendRequestData(data->data());
            }
            if (data->isEndStream()) {
                stream->markRequestCompleted();
                events |= Http2StreamEvent::RequestComplete;
            }
        }
        markStreamActive(stream, events);
    }

    void appendStreamDataAndMarkEvents(const Http2Stream::ptr& stream,
                                       std::string_view data,
                                       bool end_stream) {
        auto events = Http2StreamEvent::DataArrived;
        if (m_conn.isClient()) {
            stream->appendResponseData(std::string(data));
            if (end_stream) {
                stream->markResponseCompleted();
                events |= Http2StreamEvent::ResponseComplete;
            }
        } else {
            stream->appendRequestData(data);
            if (end_stream) {
                stream->markRequestCompleted();
                events |= Http2StreamEvent::RequestComplete;
            }
        }
        markStreamActive(stream, events);
    }

    std::expected<Http2Frame::uptr, Http2ErrorCode> materializeFrameView(
        const Http2RawFrameView& frame_view) {
        auto bytes = frame_view.bytes();
        return Http2FrameParser::parseFrame(
            reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
    }

    bool handleRawHeadersFrameView(const Http2RawFrameView& frame_view, uint32_t stream_id) {
        if ((frame_view.header.flags & Http2FrameFlags::kPadded) != 0 ||
            (frame_view.header.flags & Http2FrameFlags::kPriority) != 0) {
            return false;
        }

        auto stream = findOrCreateHeadersStream(stream_id);
        if (!stream) {
            return true;
        }

        if (!stream->canReceiveHeaders()) {
            enqueueRstStreamAction(stream_id, Http2ErrorCode::StreamClosed);
            return true;
        }

        const bool end_headers = frame_view.endHeaders();
        const bool end_stream = frame_view.endStream();
        stream->onHeadersReceived(end_stream);
        stream->appendHeaderBlock(frame_view.payload());

        if (end_headers) {
            completeReceivedHeaders(stream, end_stream);
        } else {
            m_conn.setExpectingContinuation(true, stream_id);
        }

        tryRetireClientStream(stream);
        return true;
    }

    bool handleRawContinuationFrameView(const Http2RawFrameView& frame_view, uint32_t stream_id) {
        auto stream = findAttachedStream(stream_id);
        if (!stream) {
            enqueueGoawayAction(Http2ErrorCode::ProtocolError);
            return true;
        }

        stream->appendHeaderBlock(frame_view.payload());

        if (frame_view.endHeaders()) {
            completeReceivedHeaders(stream, stream->isEndStreamReceived());
        }

        return true;
    }

    bool handleRawDataFrameView(const Http2RawFrameView& frame_view, uint32_t stream_id) {
        if ((frame_view.header.flags & Http2FrameFlags::kPadded) != 0) {
            return false;
        }

        if (stream_id == 0) {
            enqueueGoawayAction(Http2ErrorCode::ProtocolError);
            return true;
        }

        auto stream = findAttachedStream(stream_id);
        if (!stream) {
            enqueueRstStreamAction(stream_id, Http2ErrorCode::StreamClosed);
            return true;
        }
        if (!stream->canReceiveData()) {
            enqueueRstStreamAction(stream_id, Http2ErrorCode::StreamClosed);
            return true;
        }

        const auto payload = frame_view.payload();
        const int32_t data_size = static_cast<int32_t>(payload.size());
        stream->onDataReceived(frame_view.endStream());

        m_conn.adjustConnRecvWindow(-data_size);
        stream->adjustRecvWindow(-data_size);
        applyRecvWindowUpdate(stream, stream_id, payload.size());
        appendStreamDataAndMarkEvents(stream, payload, frame_view.endStream());

        tryRetireClientStream(stream);
        return true;
    }

    bool tryDispatchServerActiveFrameView(Http2RawFrameView&& frame_view) {
        if (!m_active_conn_mode || m_conn.isClient()) {
            return false;
        }

        const uint32_t stream_id = frame_view.streamId();

        if (frame_view.isHeaders()) {
            return handleRawHeadersFrameView(frame_view, stream_id);
        }

        if (frame_view.isContinuation()) {
            return handleRawContinuationFrameView(frame_view, stream_id);
        }

        if (frame_view.isData()) {
            return handleRawDataFrameView(frame_view, stream_id);
        }

        return false;
    }

    void handleHeadersFrame(Http2Frame::uptr frame, uint32_t stream_id) {
        auto stream = findOrCreateHeadersStream(stream_id);
        if (!stream) {
            return;
        }

        if (!stream->canReceiveHeaders()) {
            enqueueRstStreamAction(stream_id, Http2ErrorCode::StreamClosed);
            return;
        }

        auto* hdrs = frame->asHeaders();
        if (hdrs->hasPriority()) {
            stream->setPriority(hdrs->exclusive(), hdrs->streamDependency(), hdrs->weight());
        }

        const bool end_headers = hdrs->isEndHeaders();
        const bool end_stream = hdrs->isEndStream();
        stream->onHeadersReceived(end_stream);
        stream->appendHeaderBlock(hdrs->headerBlock());

        if (end_headers) {
            completeReceivedHeaders(stream, end_stream);
        } else {
            m_conn.setExpectingContinuation(true, stream_id);
        }

        pushStreamFrameIfNeeded(stream, std::move(frame));
        tryRetireClientStream(stream);
    }

    void handleContinuationFrame(Http2Frame::uptr frame, uint32_t stream_id) {
        auto stream = findAttachedStream(stream_id);
        if (!stream) {
            enqueueGoawayAction(Http2ErrorCode::ProtocolError);
            return;
        }

        auto* cont = frame->asContinuation();
        stream->appendHeaderBlock(cont->headerBlock());

        if (cont->isEndHeaders()) {
            completeReceivedHeaders(stream, stream->isEndStreamReceived());
        }

        pushStreamFrameIfNeeded(stream, std::move(frame));
    }

    void handleDataFrame(Http2Frame::uptr frame, uint32_t stream_id) {
        if (stream_id == 0) {
            enqueueGoawayAction(Http2ErrorCode::ProtocolError);
            return;
        }

        auto stream = findAttachedStream(stream_id);
        if (!stream) {
            enqueueRstStreamAction(stream_id, Http2ErrorCode::StreamClosed);
            return;
        }
        if (!stream->canReceiveData()) {
            enqueueRstStreamAction(stream_id, Http2ErrorCode::StreamClosed);
            return;
        }

        auto* data = frame->asData();
        const int32_t data_size = static_cast<int32_t>(data->data().size());
        stream->onDataReceived(data->isEndStream());

        m_conn.adjustConnRecvWindow(-data_size);
        stream->adjustRecvWindow(-data_size);
        applyRecvWindowUpdate(stream, stream_id, data->data().size());
        appendStreamDataAndMarkEvents(stream, data);

        pushStreamFrameIfNeeded(stream, std::move(frame));
        tryRetireClientStream(stream);
    }

    void handlePriorityFrame(Http2Frame::uptr frame, uint32_t stream_id) {
        auto stream = findAttachedStream(stream_id);
        if (!stream) {
            return;
        }

        auto* prio = frame->asPriority();
        stream->setPriority(prio->exclusive(), prio->streamDependency(), prio->weight());
    }

    void handleRstStreamFrame(Http2Frame::uptr frame, uint32_t stream_id) {
        if (stream_id == 0) {
            enqueueGoawayAction(Http2ErrorCode::ProtocolError);
            return;
        }

        auto stream = findAttachedStream(stream_id);
        if (!stream) {
            return;
        }

        stream->onRstStreamReceived();
        markStreamActive(stream, Http2StreamEvent::Reset);
        pushStreamFrameIfNeeded(stream, std::move(frame));
        stream->markRequestCompleted();
        stream->markResponseCompleted();
        stream->closeFrameQueue();
        tryRetireClientStream(stream);
    }

    void handleWindowUpdateFrame(Http2Frame::uptr frame, uint32_t stream_id) {
        auto stream = findAttachedStream(stream_id);
        if (!stream) {
            return;
        }

        auto* wu = frame->asWindowUpdate();
        const uint32_t increment = wu->windowSizeIncrement();
        if (increment == 0) {
            enqueueRstStreamAction(stream_id, Http2ErrorCode::ProtocolError);
            return;
        }

        stream->adjustSendWindow(increment);
        markStreamActive(stream, Http2StreamEvent::WindowUpdated);
        pushStreamFrameIfNeeded(stream, std::move(frame));
    }

    void handlePushPromiseFrame(Http2Frame::uptr frame, uint32_t stream_id) {
        if (!m_conn.isClient()) {
            enqueueGoawayAction(Http2ErrorCode::ProtocolError);
            return;
        }

        auto* pp = frame->asPushPromise();
        const uint32_t promised_id = pp->promisedStreamId();
        auto promised_stream = findAttachedStream(promised_id);
        if (!promised_stream) {
            promised_stream = createStreamInternal(promised_id);
            promised_stream->setState(Http2StreamState::ReservedRemote);
        }

        pushStreamFrameIfNeeded(promised_stream, std::move(frame));
        if (!m_active_conn_mode) {
            queueStreamHandler(promised_stream);
        }
    }

    void dispatchStreamFrame(Http2Frame::uptr frame) {
        const uint32_t stream_id = frame->streamId();

        if (stream_id == 0) {
            return;
        }

        if (frame->isHeaders()) {
            handleHeadersFrame(std::move(frame), stream_id);
            return;
        }

        if (frame->isContinuation()) {
            handleContinuationFrame(std::move(frame), stream_id);
            return;
        }

        if (frame->isData()) {
            handleDataFrame(std::move(frame), stream_id);
            return;
        }

        if (frame->isPriority()) {
            handlePriorityFrame(std::move(frame), stream_id);
            return;
        }

        if (frame->isRstStream()) {
            handleRstStreamFrame(std::move(frame), stream_id);
            return;
        }

        if (frame->isWindowUpdate()) {
            handleWindowUpdateFrame(std::move(frame), stream_id);
            return;
        }

        if (frame->isPushPromise()) {
            handlePushPromiseFrame(std::move(frame), stream_id);
            return;
        }

    }

    /**
     * @brief 处理 dispatchStreamFrame 中标记的待处理动作（通过 channel 发送）
     */
    void processPendingActions() {
        while (!m_pending_actions.empty()) {
            auto action = m_pending_actions.front();
            m_pending_actions.pop_front();

            switch (action.type) {
                case PendingAction::Type::SendGoaway: {
                    enqueueGoaway(action.error_code);
                    break;
                }
                case PendingAction::Type::SendRstStream: {
                    auto bytes = Http2FrameBuilder::rstStreamBytes(action.stream_id, action.error_code);
                    auto stream = m_conn.getStream(action.stream_id);
                    if (stream) {
                        stream->onRstStreamSent();
                    }
                    enqueueSendBytes(std::move(bytes));
                    break;
                }
                case PendingAction::Type::SendWindowUpdate: {
                    Http2WindowUpdateFrame frame;
                    frame.header().stream_id = action.stream_id;
                    frame.setWindowSizeIncrement(action.increment);
                    enqueueSendFrame(std::move(frame));
                    break;
                }
            }
        }
    }

    /**
     * @brief 入队 GOAWAY 帧
     */
    void enqueueGoaway(Http2ErrorCode error,
                       const std::string& debug = "",
                       const Http2OutgoingFrame::WaiterPtr& waiter = nullptr,
                       std::optional<uint32_t> last_stream_id = std::nullopt) {
        Http2GoAwayFrame frame;
        uint32_t last = last_stream_id.value_or(m_conn.lastPeerStreamId());
        frame.setLastStreamId(last);
        frame.setErrorCode(error);
        if (!debug.empty()) {
            frame.setDebugData(debug);
        }
        m_conn.markGoawaySent(last, error, debug);
        enqueueSendFrame(std::move(frame), waiter);
    }

    /**
     * @brief 将新流加入待 spawn 队列
     */
    void queueStreamHandler(Http2Stream::ptr stream) {
        m_pending_spawns.push(stream);
    }

    void markStreamActive(const Http2Stream::ptr& stream, Http2StreamEvent events) {
        if (!m_active_conn_mode) {
            return;
        }
        m_active_batch.mark(stream, events);
    }

    bool shouldDeferHeadersOnlyActiveDelivery(const Http2Stream::ptr& stream,
                                              bool end_stream) const {
        if (!stream || !m_active_conn_mode || m_conn.isClient() || end_stream) {
            return false;
        }

        const auto content_length = stream->request().getHeader("content-length");
        if (content_length.empty()) {
            return false;
        }

        size_t parsed = 0;
        const auto* begin = content_length.data();
        const auto* end = begin + content_length.size();
        const auto [ptr, ec] = std::from_chars(begin, end, parsed);
        if (ec != std::errc{} || ptr != end) {
            return false;
        }
        return parsed > 0;
    }

    void closeActiveStreamQueue() {
        if (!m_active_conn_mode || m_active_stream_queue_closed) {
            return;
        }
        m_active_stream_queue_closed = true;
        m_active_stream_mailbox.close();
    }

    void pushStreamFrameIfNeeded(const Http2Stream::ptr& stream, Http2Frame::uptr frame) {
        if (!stream) {
            return;
        }
        if (m_active_conn_mode) {
            return;
        }
        stream->pushFrame(std::move(frame));
    }

    void flushActiveStreams() {
        if (!m_active_conn_mode || m_active_batch.empty()) {
            return;
        }

        auto ready = m_active_batch.takeReady();
        m_active_stream_mailbox.sendBatch(std::move(ready));
    }

    void tryRetireClientStream(const Http2Stream::ptr& stream) {
        if (!stream || !m_conn.isClient()) {
            return;
        }
        if (!stream->isResponseCompleted()) {
            return;
        }
        if (stream->state() != Http2StreamState::Closed) {
            return;
        }
        m_conn.removeStream(stream->streamId());
    }

    Http2Stream::ptr createStreamInternal(uint32_t stream_id) {
        drainRetiredStreams();
        Http2Stream::ptr stream;
        if (m_active_conn_mode && !m_conn.isClient()) {
            stream = m_conn.createStream(stream_id, m_stream_pool.acquire(stream_id));
        } else {
            stream = m_conn.createStream(stream_id);
        }
        attachStreamIO(stream);
        rememberHotStream(stream);
        return stream;
    }

    void attachStreamIO(const Http2Stream::ptr& stream) {
        if (!stream) return;
        auto* encoder = &m_conn.encoder();
        auto* decoder = &m_conn.decoder();
        if (stream->m_io_attached &&
            stream->m_send_channel == &m_send_channel &&
            stream->m_encoder == encoder &&
            stream->m_decoder == decoder) {
            return;
        }
        stream->attachIO(&m_send_channel, encoder, decoder);
        if (m_active_conn_mode && !m_conn.isClient()) {
            stream->setRetireCallback([this](uint32_t stream_id) {
                enqueueRetireStream(stream_id);
            });
        } else {
            stream->setRetireCallback(nullptr);
        }
    }

    void rememberHotStream(const Http2Stream::ptr& stream) {
        if (!stream || !m_active_conn_mode || m_conn.isClient()) {
            return;
        }
        m_hot_stream = stream;
    }

    void clearHotStream(uint32_t stream_id) {
        if (m_hot_stream && m_hot_stream->streamId() == stream_id) {
            m_hot_stream.reset();
        }
    }

    Http2ConnImpl<SocketType>& m_conn;
    bool m_started = false;
    bool m_running;
    galay::kernel::AsyncWaiter<void> m_stop_waiter;
    galay::kernel::AsyncWaiter<void> m_writer_ready;
    galay::kernel::AsyncWaiter<void> m_writer_done;
    galay::kernel::AsyncWaiter<void> m_monitor_done;
    uint32_t m_next_local_stream_id = 0;
    std::atomic<int> m_active_handlers{0};
    std::atomic<bool> m_draining_handlers{false};
    galay::kernel::AsyncWaiter<void> m_handler_waiter;
    Http2Stream::ptr m_hot_stream;
    bool m_reject_new_streams = false;
    std::chrono::steady_clock::time_point m_last_frame_recv_at{};
    std::chrono::steady_clock::time_point m_last_ping_sent_at{};
    std::array<uint8_t, 8> m_last_ping_payload{};
    bool m_waiting_ping_ack = false;
    bool m_active_conn_mode = false;
    bool m_active_stream_queue_closed = false;
    Http2ActiveStreamBatch m_active_batch;
    Http2ActiveStreamMailbox m_active_stream_mailbox;
    Http2StreamPool m_stream_pool;

    // 发送通道：空指针表示关闭信号
    MpscChannel<Http2OutgoingFrame> m_send_channel;

    // 待处理动作队列
    std::deque<PendingAction> m_pending_actions;

    // 待 spawn 的流队列（按优先级排序）
    std::priority_queue<Http2Stream::ptr, std::vector<Http2Stream::ptr>, StreamPriorityCompare> m_pending_spawns;
    MpscChannel<uint32_t> m_retire_stream_channel;
};

// 类型别名
using Http2StreamManager = Http2StreamManagerImpl<galay::async::TcpSocket>;

} // namespace galay::http2

#endif // GALAY_HTTP2_STREAM_MANAGER_H
