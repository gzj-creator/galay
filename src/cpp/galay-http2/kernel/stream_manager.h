/**
 * @file stream_manager.h
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
#include "../protoc/http2_base.h"
#include "../protoc/http2_frame.h"
#include "../../galay-http/common/iovec_utils.h"
#include "../../galay-kernel/async/async_waiter.h"
#include "../../galay-kernel/concurrency/mpsc/unbounded_channel.h"
#include "../../galay-kernel/common/sleep.hpp"
#include "../../galay-kernel/core/runtime.h"
#include <cerrno>
#include <expected>
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
#include <fcntl.h>
#include <sys/uio.h>
#include <unistd.h>

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
        return !m_task.is_valid();
    }

    template<typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> handle) {
        auto* scheduler = handle.promise().task_ref_view().belong_scheduler();
        if (scheduler == nullptr) {
            m_scheduled = false;
            return false;
        }
        if (!schedule_task(scheduler, std::move(m_task))) {
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

inline StartDetachedTaskAwaitable start_detached_task(Task<void> task) {
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

    std::vector<Http2Stream::ptr> take_ready() {
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
            return m_mailbox->pop_batch(m_max_count);
        }

    private:
        Http2ActiveStreamMailbox* m_mailbox;
        size_t m_max_count;
    };

    void send_batch(Batch&& batch) {
        if (m_closed || batch.empty()) {
            return;
        }
        m_batches.push_back(std::move(batch));
        wake_waiter();
    }

    RecvBatchAwaitable recv_batch(
        size_t max_count = galay::spsc::UnboundedChannel<Http2Stream::ptr>::DEFAULT_BATCH_SIZE) {
        return RecvBatchAwaitable(this, max_count);
    }

    void close() {
        if (m_closed) {
            return;
        }
        m_closed = true;
        wake_waiter();
    }

    void reset() {
        m_batches.clear();
        m_closed = false;
        m_hasWaiter = false;
        m_waiter = {};
    }

private:
    friend class RecvBatchAwaitable;

    BatchResult pop_batch(size_t max_count) {
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

    void wake_waiter() {
        if (!m_hasWaiter) {
            return;
        }
        m_hasWaiter = false;
        auto waiter = std::move(m_waiter);
        m_waiter = {};
        waiter.wake_up();
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
            , m_recv_awaitable(ctx->m_mailbox->recv_batch(max_count))
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

    auto get_active_streams(
        size_t max_count = galay::spsc::UnboundedChannel<Http2Stream::ptr>::DEFAULT_BATCH_SIZE) {
        return GetActiveStreamsAwaitable(this, max_count);
    }

    bool is_closed() const {
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
template<typename SocketType, RingBufferBackendStrategy Strategy>
class Http2StreamManagerImpl
{
public:
    Http2StreamManagerImpl(Http2ConnImpl<SocketType, Strategy>& conn)
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
        prepare_for_start(false);
        if constexpr (is_ssl_socket_v<SocketType>) {
            if (!co_await start_detached_task(monitor_loop_then_notify(&m_monitor_done))) {
                mark_detached_startup_failed(true, false);
                co_return;
            }
            m_writer_ready.notify();
            co_await ssl_service_loop(std::move(handler));
            co_await finish_foreground_ssl_run();
            co_return;
        }
        if (!co_await start_background_loops()) {
            co_return;
        }
        co_await reader_loop(std::move(handler));
        co_await finish_foreground_run();
        co_return;
    }

    Task<void> start(Http2ActiveConnHandler handler) {
        prepare_for_start(true);
        if constexpr (is_ssl_socket_v<SocketType>) {
            if (!co_await start_detached_task(monitor_loop_then_notify(&m_monitor_done))) {
                mark_detached_startup_failed(true, false);
                co_return;
            }
            m_writer_ready.notify();

            Http2ConnContext ctx(m_active_stream_mailbox);
            m_active_handlers.fetch_add(1, std::memory_order_acq_rel);
            if (!co_await start_detached_task(run_active_handler(std::move(handler), &ctx))) {
                finish_active_handler_slot();
                close_active_stream_queue();
                m_conn.initiate_close();
                co_await finish_foreground_ssl_run();
                co_return;
            }

            co_await ssl_service_loop(nullptr);
            co_await finish_foreground_ssl_run();
            co_return;
        }

        if (!co_await start_background_loops()) {
            co_return;
        }

        Http2ConnContext ctx(m_active_stream_mailbox);
        m_active_handlers.fetch_add(1, std::memory_order_acq_rel);
        if (!co_await start_detached_task(run_active_handler(std::move(handler), &ctx))) {
            finish_active_handler_slot();
            close_active_stream_queue();
            m_conn.initiate_close();
            co_await finish_foreground_run();
            co_return;
        }

        co_await reader_loop(nullptr);
        co_await finish_foreground_run();
        co_return;
    }

    /**
     * @brief 将帧入队发送
     */
    void enqueue_send_frame(Http2Frame::uptr frame,
                          const Http2OutgoingFrame::WaiterPtr& waiter = nullptr) {
        enqueue_outgoing_item(Http2OutgoingFrame{std::move(frame), waiter});
    }

    void enqueue_send_bytes(std::string bytes,
                          const Http2OutgoingFrame::WaiterPtr& waiter = nullptr) {
        enqueue_outgoing_item(Http2OutgoingFrame{std::move(bytes), waiter});
    }

    template<typename FrameType>
    void enqueue_send_frame(FrameType&& frame,
                          const Http2OutgoingFrame::WaiterPtr& waiter = nullptr) {
        using FrameT = std::decay_t<FrameType>;
        static_assert(std::is_base_of_v<Http2Frame, FrameT>, "FrameType must derive from Http2Frame");
        enqueue_outgoing_item(
            Http2OutgoingFrame{std::make_unique<FrameT>(std::forward<FrameType>(frame)), waiter});
    }

    bool is_running() const { return m_running; }

    /**
     * @brief 获取连接引用（供用户 handler 使用）
     */
    Http2ConnImpl<SocketType, Strategy>& conn() { return m_conn; }

    /**
     * @brief 从非协程上下文启动 StreamManager
     * @param scheduler 当前 IO 调度器
     * @param handler 用户流处理回调
     * @details 通过 schedule_task(scheduler, ) 启动 reader/writer/monitor，
     *          不需要协程上下文，可从 CustomAwaitable::await_resume() 等普通函数调用。
     */
    bool start_with_scheduler(galay::kernel::Scheduler* scheduler, Http2StreamHandler handler) {
        prepare_for_start(false);
        if (scheduler == nullptr) {
            mark_detached_startup_failed(true, true);
            return false;
        }

        if constexpr (is_ssl_socket_v<SocketType>) {
            if (!schedule_task(scheduler, monitor_loop_then_notify(&m_monitor_done))) {
                mark_detached_startup_failed(true, false);
                return false;
            }
            m_writer_ready.notify();
            if (!schedule_task(scheduler, ssl_service_loop_then_cleanup(std::move(handler)))) {
                m_running = false;
                close_active_stream_queue();
                m_stop_waiter.notify();
                return false;
            }
            return true;
        }

        bool writer_started = schedule_task(scheduler, writer_loop_then_notify(&m_writer_done));
        bool monitor_started = schedule_task(scheduler, monitor_loop_then_notify(&m_monitor_done));
        if (!writer_started || !monitor_started) {
            if (!writer_started) {
                m_writer_done.notify();
            }
            if (!monitor_started) {
                m_monitor_done.notify();
            }
            m_running = false;
            if (writer_started) {
                enqueue_outgoing_item(Http2OutgoingFrame{});
            }
            m_writer_ready.notify();
            close_active_stream_queue();
            m_stop_waiter.notify();
            return false;
        }

        m_writer_ready.notify();
        if (!schedule_task(scheduler, reader_loop_then_cleanup(std::move(handler)))) {
            m_running = false;
            enqueue_outgoing_item(Http2OutgoingFrame{});
            close_active_stream_queue();
            m_stop_waiter.notify();
            return false;
        }
        return true;
    }

    /**
     * @brief 自动分配 stream ID 并创建流
     * @details 客户端自动分配奇数 ID（3, 5, 7, ...），服务端自动分配偶数 ID（2, 4, 6, ...）
     */
    Http2Stream::ptr allocate_stream() {
        uint32_t id = m_next_local_stream_id;
        m_next_local_stream_id += 2;
        return new_stream(id);
    }

    /**
     * @brief 优雅关闭：发送 GOAWAY、关闭连接、等待 StreamManager 停止
     * @details 替代手动的 send_goaway + conn.close() + wait_stopped() 序列
     */
    Task<void> shutdown(Http2ErrorCode error = Http2ErrorCode::NoError) {
        return shutdown_impl(error);
    }

private:
    Task<void> shutdown_impl(Http2ErrorCode error = Http2ErrorCode::NoError) {
        if (!m_started) co_return;

        if (m_running) {
            m_conn.set_draining(true);

            if (m_conn.is_client()) {
                m_reject_new_streams = true;
                auto waiter = send_goaway(error);
                if (waiter) {
                    co_await waiter->wait();
                }
            } else {
                // RFC 推荐的 graceful shutdown：先发 MAX_INT，再发真实 last_stream_id。
                auto first = send_goaway(error, "draining", kMaxStreamId);
                if (first) {
                    co_await first->wait();
                }

                auto rtt = m_conn.runtime_config().graceful_shutdown_rtt;
                if (rtt.count() > 0) {
                    co_await galay::kernel::sleep(rtt);
                }

                m_reject_new_streams = true;
                auto last_accepted = m_conn.last_peer_stream_id();
                auto second = send_goaway(error, "", last_accepted);
                if (second) {
                    co_await second->wait();
                }
            }

            // 服务端等待活跃流处理完成，避免直接断开造成业务中断。
            auto deadline = std::chrono::steady_clock::now() + m_conn.runtime_config().graceful_shutdown_timeout;
            while (m_active_handlers.load(std::memory_order_acquire) > 0 &&
                   std::chrono::steady_clock::now() < deadline) {
                co_await galay::kernel::sleep(std::chrono::milliseconds(5));
            }

            // 批量发送 RST_STREAM 给所有未完成的流
            std::vector<Http2OutgoingFrame> rst_frames;
            m_conn.for_each_stream([&](uint32_t stream_id, Http2Stream::ptr& stream) {
                if (stream && stream->state() != Http2StreamState::Closed) {
                    auto bytes = Http2FrameBuilder::rst_stream_bytes(stream_id, Http2ErrorCode::NoError);
                    stream->on_rst_stream_sent();
                    rst_frames.push_back(Http2OutgoingFrame{std::move(bytes), nullptr});
                }
            });

            // 批量入队 RST_STREAM 帧
            for (auto& frame : rst_frames) {
                enqueue_outgoing_item(std::move(frame));
            }

            // 等待 RST_STREAM 帧发送完成
            if (!rst_frames.empty()) {
                co_await galay::kernel::sleep(std::chrono::milliseconds(10));
            }

            // 关闭所有流的帧队列
            m_conn.for_each_stream([](uint32_t, Http2Stream::ptr& stream) {
                stream->close_frame_queue();
            });

            // 先只触发 transport shutdown，保留现有 awaitable，
            // 让 reader_loop 从 read_frames_batch() 正常收到 closing/peer-closed 并退出。
            m_conn.initiate_close();
            if (m_running) {
                co_await wait_stopped();
            }
            // reader_loop/Writer 全部退出后再真正 close fd，避免提前移除底层 READ/CUSTOM awaitable。
            co_await m_conn.close();
        }
        co_return;
    }

    void prepare_for_start(bool active_conn_mode) {
        m_started = true;
        m_running = true;
        m_active_conn_mode = active_conn_mode;
        m_active_stream_queue_closed = false;
        m_active_stream_mailbox.reset();
        m_draining_handlers.store(false, std::memory_order_release);
        m_reject_new_streams = false;
        m_last_frame_recv_at = std::chrono::steady_clock::now();
        m_waiting_ping_ack = false;
        m_static_response_batch.clear();
        m_send_channel_failed.store(false, std::memory_order_release);
        if (m_static_response_batch.capacity() < 64) {
            m_static_response_batch.reserve(64);
        }

        if (m_next_local_stream_id == 0) {
            m_next_local_stream_id = m_conn.is_client() ? 3 : 2;
        }
        if (!m_conn.is_client()) {
            const uint32_t target_window = m_conn.runtime_config().flow_control_target_window;
            const int32_t current_window = m_conn.conn_recv_window();
            if (target_window > 0 && static_cast<int32_t>(target_window) > current_window) {
                const auto increment = static_cast<uint32_t>(
                    static_cast<int64_t>(target_window) - current_window);
                enqueue_window_update_action(0, increment);
                m_conn.adjust_conn_recv_window(static_cast<int32_t>(increment));
            }
        }
        m_conn.reserve_streams(
            static_cast<size_t>(std::max<uint32_t>(m_conn.local_settings().max_concurrent_streams, 64u)) + 8);
    }

    Task<bool> start_background_loops() {
        bool writer_started = co_await start_detached_task(writer_loop_then_notify(&m_writer_done));
        if (!writer_started) {
            mark_detached_startup_failed(true, true);
            co_return false;
        }

        bool monitor_started = co_await start_detached_task(monitor_loop_then_notify(&m_monitor_done));
        if (!monitor_started) {
            m_running = false;
            enqueue_outgoing_item(Http2OutgoingFrame{});
            m_monitor_done.notify();
            m_writer_ready.notify();
            close_active_stream_queue();
            m_stop_waiter.notify();
            co_return false;
        }

        m_writer_ready.notify();
        co_return true;
    }

    Task<void> finish_foreground_run() {
        m_draining_handlers.store(true, std::memory_order_release);
        if (m_active_handlers.load(std::memory_order_acquire) > 0) {
            co_await m_handler_waiter.wait();
        }

        m_running = false;
        enqueue_outgoing_item(Http2OutgoingFrame{});
        co_await m_writer_done.wait();
        co_await m_monitor_done.wait();
        m_stop_waiter.notify();
        co_return;
    }

    Task<void> finish_foreground_ssl_run() {
        m_draining_handlers.store(true, std::memory_order_release);
        if (m_active_handlers.load(std::memory_order_acquire) > 0) {
            co_await m_handler_waiter.wait();
        }

        m_running = false;
        co_await m_monitor_done.wait();
        m_stop_waiter.notify();
        co_return;
    }

    Task<void> reader_loop_then_cleanup(Http2StreamHandler handler) {
        co_await reader_loop(std::move(handler));

        m_draining_handlers.store(true, std::memory_order_release);
        if (m_active_handlers.load(std::memory_order_acquire) > 0) {
            co_await m_handler_waiter.wait();
        }

        m_running = false;
        enqueue_outgoing_item(Http2OutgoingFrame{});
        co_await m_writer_done.wait();
        co_await m_monitor_done.wait();
        m_stop_waiter.notify();
        co_return;
    }

    Task<void> ssl_service_loop_then_cleanup(Http2StreamHandler handler) {
        co_await ssl_service_loop(std::move(handler));

        m_draining_handlers.store(true, std::memory_order_release);
        if (m_active_handlers.load(std::memory_order_acquire) > 0) {
            co_await m_handler_waiter.wait();
        }

        m_running = false;
        co_await m_monitor_done.wait();
        m_stop_waiter.notify();
        co_return;
    }

    void mark_detached_startup_failed(bool monitor_done, bool writer_done) {
        m_running = false;
        if (monitor_done) {
            m_monitor_done.notify();
        }
        if (writer_done) {
            m_writer_done.notify();
        }
        m_writer_ready.notify();
        close_active_stream_queue();
        m_stop_waiter.notify();
    }

    void finish_active_handler_slot() {
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
    Http2OutgoingFrame::WaiterPtr send_goaway(Http2ErrorCode error = Http2ErrorCode::NoError,
                                             const std::string& debug = "",
                                             std::optional<uint32_t> last_stream_id = std::nullopt) {
        auto waiter = std::make_shared<Http2OutgoingFrame::Waiter>();
        enqueue_goaway(error, debug, waiter, last_stream_id);
        return waiter;
    }

    /**
     * @brief 等待 StreamManager 停止（start() 完成）
     */
    galay::kernel::AsyncWaiterAwaitable<void> wait_stopped() {
        return m_stop_waiter.wait();
    }

private:
    static constexpr size_t kSslIoOwnerBatchSize = 64;
    static constexpr size_t kMaxWritevIovecs = 1024;
    static constexpr auto kSslIoOwnerHotWaitInterval = std::chrono::milliseconds(1);
    static constexpr auto kSslIoOwnerActivePollInterval = std::chrono::milliseconds(5);
    static constexpr auto kSslIoOwnerIdlePollInterval = std::chrono::milliseconds(50);
    static constexpr auto kWriterStopPollInterval = std::chrono::milliseconds(50);

    void enqueue_outgoing_item(Http2OutgoingFrame&& item) {
        auto waiter = item.waiter;
        if (m_send_channel.send(std::move(item))) {
            return;
        }
        if (waiter) {
            waiter->notify();
        }
        m_send_channel_failed.store(true, std::memory_order_release);
        m_conn.initiate_close();
    }

    void enqueue_outgoing_batch(std::vector<Http2OutgoingFrame>&& items) {
        if (m_send_channel.send_batch(std::move(items))) {
            return;
        }
        for (auto& item : items) {
            if (item.waiter) {
                item.waiter->notify();
            }
        }
        m_send_channel_failed.store(true, std::memory_order_release);
        m_conn.initiate_close();
    }

    void collect_outgoing_frame(Http2OutgoingFrame&& item,
                              std::vector<Http2OutgoingFrame>& outgoing_batch,
                              std::vector<Http2OutgoingFrame::WaiterPtr>& waiters,
                              bool& has_shutdown) {
        if (item.is_empty()) {
            has_shutdown = true;
            return;
        }
        if (item.waiter) {
            waiters.push_back(item.waiter);
        }
        outgoing_batch.push_back(std::move(item));
    }

    void drain_outgoing_channel(std::vector<Http2OutgoingFrame>& outgoing_batch,
                              std::vector<Http2OutgoingFrame::WaiterPtr>& waiters,
                              bool& has_shutdown) {
        while (!has_shutdown) {
            auto item = m_send_channel.try_recv();
            if (!item) {
                break;
            }
            collect_outgoing_frame(std::move(*item), outgoing_batch, waiters, has_shutdown);
        }
    }

    void notify_waiters(std::vector<Http2OutgoingFrame::WaiterPtr>& waiters) {
        for (auto& waiter : waiters) {
            if (waiter) {
                waiter->notify();
            }
        }
    }

    bool should_hot_wait_for_outgoing(bool had_ready_active_streams,
                                  bool had_pending_spawns) const {
        if (had_ready_active_streams || had_pending_spawns) {
            return true;
        }
        if (m_active_conn_mode) {
            return false;
        }
        return m_active_handlers.load(std::memory_order_acquire) > 0;
    }

    std::chrono::milliseconds ssl_io_owner_poll_interval(bool low_latency_mode) const {
        return low_latency_mode ? kSslIoOwnerActivePollInterval : kSslIoOwnerIdlePollInterval;
    }

    bool should_enforce_settings_ack_timeout(std::chrono::steady_clock::time_point now) const {
        const auto settings_timeout = m_conn.runtime_config().settings_ack_timeout;
        if (settings_timeout.count() <= 0 || !m_conn.is_settings_ack_pending()) {
            return false;
        }
        if (now - m_conn.settings_sent_at() <= settings_timeout) {
            return false;
        }

        // Once the peer is actively sending frames after our SETTINGS, prefer to
        // keep the connection alive and validate behavior via actual frame-level
        // protocol checks instead of timing out a missing ACK.
        return m_last_frame_recv_at <= m_conn.settings_sent_at();
    }

    Task<bool> send_ssl_outgoing_batch(const std::vector<Http2OutgoingFrame>& outgoing_batch,
                                    std::vector<Http2OutgoingFrame::WaiterPtr>& waiters,
                                    std::string& coalesced_buffer) {
        if (outgoing_batch.empty()) {
            co_return true;
        }

        size_t total_bytes = 0;
        for (const auto& item : outgoing_batch) {
            total_bytes += item.serialized_size();
        }

        coalesced_buffer.clear();
        if (coalesced_buffer.capacity() < total_bytes) {
            coalesced_buffer.reserve(total_bytes);
        }
        for (const auto& item : outgoing_batch) {
            item.append_to(coalesced_buffer);
        }

        size_t offset = 0;
        while (offset < coalesced_buffer.size()) {
            auto send_result = co_await m_conn.socket().send(
                coalesced_buffer.data() + offset,
                coalesced_buffer.size() - offset);
            if (!send_result || send_result.value() == 0) {
                if (m_conn.is_closing() || m_conn.is_peer_closed() ||
                    m_conn.is_goaway_sent() || m_conn.is_goaway_received()) {
                } else if (send_result) {
                } else {
                }
                notify_waiters(waiters);
                m_conn.for_each_stream([](uint32_t, Http2Stream::ptr& stream) {
                    stream->close_frame_queue();
                });
                close_active_stream_queue();
                co_return false;
            }
            offset += send_result.value();
        }

        co_return true;
    }

    Task<void> writer_loop_then_notify(AsyncWaiter<void>* done) {
        co_await writer_loop();
        if (done) {
            done->notify();
        }
        co_return;
    }

    Task<void> monitor_loop_then_notify(AsyncWaiter<void>* done) {
        co_await monitor_loop();
        if (done) {
            done->notify();
        }
        co_return;
    }

    Http2Stream::ptr new_stream(uint32_t stream_id) {
        auto stream = m_conn.get_stream(stream_id);
        if (stream) {
            attach_stream_io(stream);
            return stream;
        }
        return create_stream_internal(stream_id);
    }

    /**
     * @brief Reader 协程：读取帧、处理连接级帧、分发流级帧
     */
    Task<void> reader_loop(Http2StreamHandler handler) {
        // reader_loop 只在 IO 错误（peer closed / connection error）或连接关闭时退出。
        // GOAWAY（无论收到还是发出）不退出：GOAWAY 只表示不再有新流，
        // 已有流的帧仍需继续读取直到连接关闭（RFC 9113 §6.8）。
        while (true) {
            if constexpr (!is_ssl_socket_v<SocketType>) {
                if (m_active_conn_mode && !m_conn.is_client()) {
                    auto frame_views_result = co_await m_conn.read_frame_views_batch();

                    if (!frame_views_result) {
                        if (m_conn.is_closing() || m_conn.is_peer_closed()) {
                            break;
                        }
                        if (frame_views_result.error() == Http2ErrorCode::NoError) {
                            continue;
                        }
                        if (frame_views_result.error() == Http2ErrorCode::ProtocolError &&
                            (m_conn.is_peer_closed() || m_conn.is_closing())) {
                            break;
                        }
                        if (m_conn.last_read_error().empty()) {
                        } else {
                        }
                        enqueue_goaway(frame_views_result.error());
                        break;
                    }

                    bool exit_loop = false;
                    auto& frame_views = *frame_views_result;
                    for (auto& frame_view : frame_views) {
                        const uint32_t stream_id = frame_view.stream_id();
                        m_last_frame_recv_at = std::chrono::steady_clock::now();


                        if (m_conn.is_expecting_continuation()) {
                            if (!frame_view.is_continuation() ||
                                stream_id != m_conn.continuation_stream_id()) {
                                enqueue_goaway(Http2ErrorCode::ProtocolError);
                                exit_loop = true;
                                break;
                            }
                        }

                        if (frame_view.is_connection_frame()) {
                            auto frame = materialize_frame_view(frame_view);
                            if (!frame) {
                                enqueue_goaway(frame.error());
                                exit_loop = true;
                                break;
                            }
                            handle_connection_frame(std::move(*frame));
                            continue;
                        }

                        if (try_dispatch_server_active_frame_view(std::move(frame_view))) {
                            continue;
                        }

                        auto frame = materialize_frame_view(frame_view);
                        if (!frame) {
                            enqueue_goaway(frame.error());
                            exit_loop = true;
                            break;
                        }
                        dispatch_stream_frame(std::move(*frame));
                    }

                    flush_static_response_batch();
                    process_pending_actions();
                    drain_retired_streams();
                    flush_active_streams();

                    while (!m_pending_spawns.empty()) {
                        auto stream = m_pending_spawns.top();
                        m_pending_spawns.pop();
                        m_active_handlers.fetch_add(1, std::memory_order_acq_rel);
                        if (!co_await start_detached_task(run_handler(handler, stream))) {
                            if (stream) {
                                stream->close_frame_queue();
                                enqueue_retire_stream(stream->stream_id());
                            }
                            finish_active_handler_slot();
                        }
                    }

                    if (exit_loop) {
                        break;
                    }
                    continue;
                }
            }

            auto frames_result = co_await m_conn.read_frames_batch();

            if (!frames_result) {
                if (m_conn.is_closing() || m_conn.is_peer_closed()) {
                    break;
                }
                if (frames_result.error() == Http2ErrorCode::NoError) {
                    continue;
                }
                if (frames_result.error() == Http2ErrorCode::ProtocolError &&
                    (m_conn.is_peer_closed() || m_conn.is_closing())) {
                    break;
                }
                if (m_conn.last_read_error().empty()) {
                } else {
                }
                enqueue_goaway(frames_result.error());
                break;
            }

            bool exit_loop = false;
            auto& frames = *frames_result;
            for (auto& frame : frames) {
                uint32_t stream_id = frame->stream_id();
                m_last_frame_recv_at = std::chrono::steady_clock::now();


                // CONTINUATION 状态检查
                if (m_conn.is_expecting_continuation()) {
                    if (!frame->is_continuation() || stream_id != m_conn.continuation_stream_id()) {
                        enqueue_goaway(Http2ErrorCode::ProtocolError);
                        exit_loop = true;
                        break;
                    }
                }

                // 连接级帧
                if (frame->is_settings() || frame->is_ping() || frame->is_go_away() ||
                    (frame->is_window_update() && stream_id == 0)) {
                    handle_connection_frame(std::move(frame));
                    continue;
                }

                // 流级帧 → 分发到 Http2Stream 帧队列
                dispatch_stream_frame(std::move(frame));
            }

            // 处理 dispatch_stream_frame 中标记的待处理动作
            process_pending_actions();
            flush_active_streams();

            // spawn 待处理的流 handler
            while (!m_pending_spawns.empty()) {
                auto stream = m_pending_spawns.top();
                m_pending_spawns.pop();
                m_active_handlers.fetch_add(1, std::memory_order_acq_rel);
                if (!co_await start_detached_task(run_handler(handler, stream))) {
                    if (stream) {
                        stream->close_frame_queue();
                        enqueue_retire_stream(stream->stream_id());
                    }
                    finish_active_handler_slot();
                }
            }

            if (exit_loop) {
                break;
            }
        }
        // 关闭所有流的帧队列
        m_conn.for_each_stream([](uint32_t, Http2Stream::ptr& stream) {
            stream->close_frame_queue();
        });
        close_active_stream_queue();
        drain_retired_streams();

        co_return;
    }

    Task<void> ssl_service_loop(Http2StreamHandler handler) {
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
            drain_outgoing_channel(outgoing_batch, waiters, has_shutdown);

            if (!outgoing_batch.empty()) {
                if (!co_await send_ssl_outgoing_batch(outgoing_batch, waiters, coalesced_buffer)) {
                    co_return;
                }
            }
            notify_waiters(waiters);

            bool exit_loop = false;
            bool had_ready_active_streams = false;
            bool had_pending_spawns = false;
            while (!exit_loop) {
                if (m_active_conn_mode && !m_conn.is_client()) {
                    auto frame_views_result = detail::parse_buffered_frame_view_batch(
                        m_conn.ring_buffer(),
                        m_conn.peer_settings().max_frame_size,
                        std::numeric_limits<size_t>::max());
                    if (!frame_views_result) {
                        if (m_conn.is_closing() || m_conn.is_peer_closed()) {
                            exit_loop = true;
                            break;
                        }
                        if (frame_views_result.error() == Http2ErrorCode::NoError) {
                            break;
                        }
                        if (m_conn.last_read_error().empty()) {
                        } else {
                        }
                        enqueue_goaway(frame_views_result.error());
                        exit_loop = true;
                        break;
                    }
                    if (frame_views_result->empty()) {
                        break;
                    }

                    auto& frame_views = *frame_views_result;
                    for (auto& frame_view : frame_views) {
                        const uint32_t stream_id = frame_view.stream_id();
                        m_last_frame_recv_at = std::chrono::steady_clock::now();


                        if (m_conn.is_expecting_continuation()) {
                            if (!frame_view.is_continuation() ||
                                stream_id != m_conn.continuation_stream_id()) {
                                enqueue_goaway(Http2ErrorCode::ProtocolError);
                                exit_loop = true;
                                break;
                            }
                        }

                        if (frame_view.is_connection_frame()) {
                            auto frame = materialize_frame_view(frame_view);
                            if (!frame) {
                                enqueue_goaway(frame.error());
                                exit_loop = true;
                                break;
                            }
                            handle_connection_frame(std::move(*frame));
                            continue;
                        }

                        if (try_dispatch_server_active_frame_view(std::move(frame_view))) {
                            continue;
                        }

                        auto frame = materialize_frame_view(frame_view);
                        if (!frame) {
                            enqueue_goaway(frame.error());
                            exit_loop = true;
                            break;
                        }
                        dispatch_stream_frame(std::move(*frame));
                    }

                    flush_static_response_batch();
                    had_ready_active_streams = had_ready_active_streams || !m_active_batch.empty();
                    process_pending_actions();
                    flush_active_streams();
                    if (exit_loop) {
                        break;
                    }
                    continue;
                }

                auto frames_result = detail::parse_buffered_frame_batch(
                    m_conn.ring_buffer(),
                    m_conn.peer_settings().max_frame_size,
                    std::numeric_limits<size_t>::max(),
                    frame_scratch);
                if (!frames_result) {
                    if (m_conn.is_closing() || m_conn.is_peer_closed()) {
                        exit_loop = true;
                        break;
                    }
                    if (frames_result.error() == Http2ErrorCode::NoError) {
                        break;
                    }
                    if (m_conn.last_read_error().empty()) {
                    } else {
                    }
                    enqueue_goaway(frames_result.error());
                    exit_loop = true;
                    break;
                }
                if (frames_result->empty()) {
                    break;
                }

                auto& frames = *frames_result;
                for (auto& frame : frames) {
                    uint32_t stream_id = frame->stream_id();
                    m_last_frame_recv_at = std::chrono::steady_clock::now();


                    if (m_conn.is_expecting_continuation()) {
                        if (!frame->is_continuation() || stream_id != m_conn.continuation_stream_id()) {
                            enqueue_goaway(Http2ErrorCode::ProtocolError);
                            exit_loop = true;
                            break;
                        }
                    }

                    if (frame->is_settings() || frame->is_ping() || frame->is_go_away() ||
                        (frame->is_window_update() && stream_id == 0)) {
                        handle_connection_frame(std::move(frame));
                        continue;
                    }

                    dispatch_stream_frame(std::move(frame));
                }

                had_ready_active_streams =
                    had_ready_active_streams || (m_active_conn_mode && !m_active_batch.empty());
                process_pending_actions();
                drain_retired_streams();
                flush_active_streams();

                had_pending_spawns = had_pending_spawns || !m_pending_spawns.empty();
                while (!m_pending_spawns.empty()) {
                    auto stream = m_pending_spawns.top();
                    m_pending_spawns.pop();
                    m_active_handlers.fetch_add(1, std::memory_order_acq_rel);
                    if (!co_await start_detached_task(run_handler(handler, stream))) {
                        if (stream) {
                            stream->close_frame_queue();
                            enqueue_retire_stream(stream->stream_id());
                        }
                        finish_active_handler_slot();
                    }
                }
            }

            if (exit_loop || has_shutdown) {
                break;
            }

            if (!m_send_channel.empty()) {
                drain_retired_streams();
                continue;
            }

            const bool hot_wait_outgoing =
                should_hot_wait_for_outgoing(had_ready_active_streams, had_pending_spawns);
            if (hot_wait_outgoing) {
                auto hot_batch_result = co_await m_send_channel.recv_batch(kSslIoOwnerBatchSize)
                    .timeout(kSslIoOwnerHotWaitInterval);
                if (hot_batch_result) {
                    outgoing_batch.clear();
                    waiters.clear();
                    has_shutdown = false;

                    for (auto& item : *hot_batch_result) {
                        collect_outgoing_frame(std::move(item), outgoing_batch, waiters, has_shutdown);
                        if (has_shutdown) {
                            break;
                        }
                    }
                    drain_outgoing_channel(outgoing_batch, waiters, has_shutdown);

                    if (!outgoing_batch.empty()) {
                        if (!co_await send_ssl_outgoing_batch(outgoing_batch, waiters, coalesced_buffer)) {
                            co_return;
                        }
                    }
                    notify_waiters(waiters);
                    if (has_shutdown) {
                        break;
                    }
                    drain_retired_streams();
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
            auto write_iovecs = borrow_write_iovecs(m_conn.ring_buffer());
            const struct iovec* first_write_iov = IoVecWindow::first_non_empty(write_iovecs);
            if (first_write_iov == nullptr) {
                m_conn.set_last_read_error("RingBuffer is full");
                enqueue_goaway(Http2ErrorCode::ProtocolError);
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
                m_conn.set_last_read_error("RingBuffer is full");
                enqueue_goaway(Http2ErrorCode::ProtocolError);
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
                    .timeout(ssl_io_owner_poll_interval(hot_wait_outgoing)));
            if (!recv_result) {
                const auto& error = recv_result.error();
#ifdef GALAY_SSL_FEATURE_ENABLED
                if (error.code() == galay::ssl::SslErrorCode::kTimeout) {
                    continue;
                }
                if (error.code() == galay::ssl::SslErrorCode::kPeerClosed) {
                    m_conn.mark_peer_closed(error.message());
                    break;
                }
#endif
                if (m_conn.is_closing()) {
                    m_conn.set_last_read_error(error.message());
                    break;
                }
                m_conn.set_last_read_error(error.message());
                enqueue_goaway(Http2ErrorCode::ProtocolError);
                break;
            }

            const size_t bytes_read = recv_result->size();
            if (bytes_read == 0) {
                m_conn.mark_peer_closed();
                break;
            }

            m_conn.clear_last_read_error();
            if (staged_recv) {
                m_conn.feed_data(recv_scratch.data(), bytes_read);
            } else {
                m_conn.ring_buffer().produce(bytes_read);
            }
        }

        m_conn.for_each_stream([](uint32_t, Http2Stream::ptr& stream) {
            stream->close_frame_queue();
        });
        close_active_stream_queue();
        drain_retired_streams();
        co_return;
    }

    Task<void> run_handler(Http2StreamHandler handler, Http2Stream::ptr stream) {
        co_await handler(stream);
        // Handler 可能在非 IO owner 线程恢复，流表回收统一回送给主循环串行处理。
        enqueue_retire_stream(stream ? stream->stream_id() : 0);
        int remaining = m_active_handlers.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (remaining == 0 && m_draining_handlers.load(std::memory_order_acquire)) {
            m_handler_waiter.notify();
        }
        co_return;
    }

    Task<void> run_active_handler(Http2ActiveConnHandler handler, Http2ConnContext* ctx) {
        co_await handler(*ctx);
        if (m_running && !m_conn.is_closing()) {
            m_conn.initiate_close();
        }
        int remaining = m_active_handlers.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (remaining == 0 && m_draining_handlers.load(std::memory_order_acquire)) {
            m_handler_waiter.notify();
        }
        co_return;
    }

    Task<void> monitor_loop() {
        while (m_running) {
            co_await galay::kernel::sleep(std::chrono::milliseconds(100));
            if (!m_running) {
                break;
            }

            auto now = std::chrono::steady_clock::now();

            if (should_enforce_settings_ack_timeout(now)) {
                enqueue_goaway(Http2ErrorCode::SettingsTimeout, "SETTINGS ACK timeout");
                m_conn.initiate_close();
                break;
            }

            if (!m_conn.runtime_config().ping_enabled ||
                m_conn.runtime_config().ping_interval.count() <= 0) {
                continue;
            }

            if (!m_waiting_ping_ack) {
                if (now - m_last_frame_recv_at >= m_conn.runtime_config().ping_interval) {
                    Http2PingFrame ping;
                    m_last_ping_payload.fill(0);
                    auto nonce = static_cast<uint64_t>(
                        std::chrono::duration_cast<std::chrono::microseconds>(
                            now.time_since_epoch()).count());
                    for (int i = 0; i < 8; ++i) {
                        m_last_ping_payload[7 - i] = static_cast<uint8_t>((nonce >> (i * 8)) & 0xFF);
                    }
                    ping.set_opaque_data(m_last_ping_payload.data());
                    enqueue_send_frame(std::move(ping));
                    m_waiting_ping_ack = true;
                    m_last_ping_sent_at = now;
                }
            } else if (m_conn.runtime_config().ping_timeout.count() > 0 &&
                       now - m_last_ping_sent_at > m_conn.runtime_config().ping_timeout) {
                enqueue_goaway(Http2ErrorCode::ProtocolError, "PING ACK timeout");
                m_conn.initiate_close();
                break;
            }
        }
        co_return;
    }

    /**
     * @brief Writer 协程：从 send channel 接收数据并写入 socket
     * @details 使用 writev 批量发送多个帧，减少系统调用和内存拷贝
     */
    Task<void> writer_loop() {
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
            auto item_result =
                co_await m_send_channel.recv().timeout(kWriterStopPollInterval);
            if (!item_result) {
                if (IOError::contains(item_result.error().code(), kTimeout) &&
                    !m_send_channel_failed.load(std::memory_order_acquire)) {
                    continue;
                }
                break;
            }

            outgoing_batch.clear();
            flattened_buffers.clear();
            write_state.clear();
            waiters.clear();

            bool has_shutdown = false;
            auto collect_item = [&](Http2OutgoingFrame&& item) {
                if (item.is_empty()) {
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
                auto next = m_send_channel.try_recv();
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
                    const size_t count = item.export_iovecs(iovecs);
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
                    // 支持 writev 的 socket（如 AsyncTcpSocket）：一次批量发送
                    while (!write_state.empty()) {
                        const auto iovec_count = std::min(write_state.count(), kMaxWritevIovecs);
                        auto result = co_await m_conn.socket().writev(
                            std::span<const struct iovec>(write_state.data(), iovec_count));
                        if (!result) {
                            if (m_conn.is_closing() || m_conn.is_peer_closed() ||
                                m_conn.is_goaway_sent() || m_conn.is_goaway_received()) {
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
                                if (m_conn.is_closing() || m_conn.is_peer_closed() ||
                                    m_conn.is_goaway_sent() || m_conn.is_goaway_received()) {
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
    void handle_connection_frame(Http2Frame::uptr frame) {
        switch (frame->type()) {
            case Http2FrameType::Settings: {
                auto* settings = frame->as_settings();
                auto err = Http2ConnImpl<SocketType, Strategy>::validate_settings_frame(*settings);
                if (err != Http2ErrorCode::NoError) {
                    enqueue_goaway_action(err);
                    return;
                }
                if (settings->is_ack()) {
                    m_conn.mark_settings_ack_received();
                } else {
                    auto next_settings = m_conn.peer_settings();
                    const uint32_t old_initial_window =
                        m_conn.peer_settings().initial_window_size;
                    err = next_settings.apply_settings(*settings);
                    if (err != Http2ErrorCode::NoError) {
                        enqueue_goaway_action(err);
                        return;
                    }
                    const int64_t initial_window_delta =
                        static_cast<int64_t>(next_settings.initial_window_size) -
                        static_cast<int64_t>(old_initial_window);
                    bool stream_window_overflow = false;
                    if (initial_window_delta != 0) {
                        m_conn.for_each_stream([&](uint32_t, Http2Stream::ptr& stream) {
                            if (!stream) {
                                return;
                            }
                            const int64_t next_window =
                                static_cast<int64_t>(stream->send_window()) +
                                initial_window_delta;
                            if (next_window > kMaxStreamId ||
                                next_window < std::numeric_limits<int32_t>::min()) {
                                stream_window_overflow = true;
                            }
                        });
                    }
                    if (stream_window_overflow) {
                        enqueue_goaway_action(Http2ErrorCode::FlowControlError);
                        return;
                    }

                    err = m_conn.apply_peer_settings(*settings);
                    if (err != Http2ErrorCode::NoError) {
                        enqueue_goaway_action(err);
                        return;
                    }
                    m_conn.for_each_stream([&](uint32_t, Http2Stream::ptr& stream) {
                        if (!stream) {
                            return;
                        }
                        if (initial_window_delta != 0) {
                            stream->adjust_send_window(static_cast<int32_t>(initial_window_delta));
                        }
                        stream->m_max_frame_size = m_conn.peer_settings().max_frame_size;
                        stream->m_max_header_list_size =
                            m_conn.peer_settings().max_header_list_size;
                        const bool made_progress = stream->flush_pending_data();
                        // made_progress only reports whether queued DATA was flushed now.
                    });

                    Http2SettingsFrame ack;
                    ack.set_ack(true);
                    enqueue_send_frame(std::move(ack));
                }
                break;
            }

            case Http2FrameType::Ping: {
                auto* ping = frame->as_ping();
                if (frame->stream_id() != 0) {
                    enqueue_goaway_action(Http2ErrorCode::ProtocolError);
                    return;
                }
                if (!ping->is_ack()) {
                    Http2PingFrame pong;
                    pong.set_opaque_data(ping->opaque_data());
                    pong.set_ack(true);
                    enqueue_send_frame(std::move(pong));
                } else if (m_waiting_ping_ack &&
                           std::memcmp(ping->opaque_data(), m_last_ping_payload.data(), 8) == 0) {
                    m_waiting_ping_ack = false;
                }
                break;
            }

            case Http2FrameType::GoAway: {
                auto* goaway = frame->as_go_away();
                m_reject_new_streams = true;
                m_conn.mark_goaway_received(
                    goaway->last_stream_id(), goaway->error_code(), goaway->debug_data());

                if (m_conn.is_client()) {
                    const uint32_t last = goaway->last_stream_id();
                    m_conn.for_each_stream([&](uint32_t stream_id, Http2Stream::ptr& stream) {
                        if (!stream || stream_id <= last) {
                            return;
                        }
                        Http2GoAwayError err;
                        err.stream_id = stream_id;
                        err.last_stream_id = last;
                        err.error_code = goaway->error_code();
                        err.retryable = true;
                        err.debug = goaway->debug_data();
                        stream->set_go_away_error(std::move(err));
                        stream->close_frame_queue();
                    });
                }
                break;
            }

            case Http2FrameType::WindowUpdate: {
                auto* wu = frame->as_window_update();
                uint32_t increment = wu->window_size_increment();
                if (increment == 0) {
                    enqueue_goaway_action(Http2ErrorCode::ProtocolError);
                    return;
                }
                if (static_cast<int64_t>(m_conn.conn_send_window()) + increment > kMaxStreamId) {
                    enqueue_goaway_action(Http2ErrorCode::FlowControlError);
                    return;
                }
                m_conn.adjust_conn_send_window(increment);
                m_conn.for_each_stream([this](uint32_t, Http2Stream::ptr& stream) {
                    if (!stream) {
                        return;
                    }
                    stream->m_max_frame_size = m_conn.peer_settings().max_frame_size;
                    stream->m_max_header_list_size =
                        m_conn.peer_settings().max_header_list_size;
                    const bool made_progress = stream->flush_pending_data();
                    // made_progress only reports whether queued DATA was flushed now.
                });
                break;
            }

            default:
                break;
        }
    }

    /**
     * @brief 分发流级帧到对应 Http2Stream 的帧队列
     */
    void enqueue_goaway_action(Http2ErrorCode error) {
        m_pending_actions.push_back({PendingAction::Type::SendGoaway, 0, error});
    }

    void enqueue_rst_stream_action(uint32_t stream_id, Http2ErrorCode error) {
        m_pending_actions.push_back({PendingAction::Type::SendRstStream, stream_id, error});
    }

    void enqueue_window_update_action(uint32_t stream_id, uint32_t increment) {
        m_pending_actions.push_back({
            PendingAction::Type::SendWindowUpdate, stream_id, Http2ErrorCode::NoError, increment});
    }

    void enqueue_retire_stream(uint32_t stream_id) {
        if (stream_id == 0) {
            return;
        }
        if (!m_retire_stream_channel.send(stream_id)) {
            m_conn.initiate_close();
        }
    }

    void drain_retired_streams() {
        while (auto stream_id = m_retire_stream_channel.try_recv()) {
            clear_hot_stream(*stream_id);
            m_conn.remove_stream(*stream_id);
        }
    }

    Http2Stream::ptr find_attached_stream(uint32_t stream_id) {
        if (m_hot_stream && m_hot_stream->stream_id() == stream_id) {
            return m_hot_stream;
        }
        auto stream = m_conn.get_stream(stream_id);
        attach_stream_io(stream);
        remember_hot_stream(stream);
        return stream;
    }

    Http2Stream::ptr find_or_create_headers_stream(uint32_t stream_id) {
        auto stream = find_attached_stream(stream_id);
        if (stream) {
            return stream;
        }

        if (m_conn.is_client()) {
            enqueue_goaway_action(Http2ErrorCode::ProtocolError);
            return nullptr;
        }

        if (m_reject_new_streams ||
            (m_conn.is_goaway_sent() && m_conn.goaway_last_stream_id() != kMaxStreamId)) {
            enqueue_rst_stream_action(stream_id, Http2ErrorCode::RefusedStream);
            return nullptr;
        }
        if (stream_id <= m_conn.last_peer_stream_id()) {
            enqueue_goaway_action(Http2ErrorCode::ProtocolError);
            return nullptr;
        }
        if (m_conn.stream_count() >= m_conn.local_settings().max_concurrent_streams) {
            enqueue_rst_stream_action(stream_id, Http2ErrorCode::RefusedStream);
            return nullptr;
        }

        stream = create_stream_internal(stream_id);
        m_conn.set_last_peer_stream_id(stream_id);
        return stream;
    }

    void decode_buffered_headers(const Http2Stream::ptr& stream) {
        auto fields = m_conn.decoder().decode(stream->header_block());
        if (fields) {
            stream->set_decoded_headers(std::move(fields.value()));
        }
        stream->clear_header_block();
        m_conn.set_expecting_continuation(false);
    }

    const H2StaticRoute* find_static_route(std::string_view path) const {
        const auto& routes = m_conn.runtime_config().static_routes;
        for (const auto& route : routes) {
            if (route.path == path) {
                return &route;
            }
        }
        return nullptr;
    }

    const H2StaticRoute* find_static_route(const Http2Request& request) const {
        return find_static_route(request.path);
    }

    const H2StaticRoute* find_static_response_route(std::string_view method,
                                                 std::string_view path) const {
        const bool is_get = method == "GET";
        const bool is_head = method == "HEAD";
        if (!is_get && !is_head) {
            return nullptr;
        }

        const auto* route = find_static_route(path);
        if (route == nullptr) {
            return nullptr;
        }
        if (is_head && !route->response.allow_head) {
            return nullptr;
        }
        return route;
    }

    bool is_static_file_method(std::string_view method) const {
        return method == "GET" || method == "HEAD";
    }

    static bool path_starts_with_mount(std::string_view path, std::string_view prefix) {
        if (prefix == "/") {
            return !path.empty() && path.front() == '/';
        }
        if (path == prefix) {
            return true;
        }
        return path.size() > prefix.size() &&
               path.compare(0, prefix.size(), prefix) == 0 &&
               path[prefix.size()] == '/';
    }

    const H2StaticFileMount* find_static_file_mount(std::string_view path) const {
        const H2StaticFileMount* best = nullptr;
        const auto& mounts = m_conn.runtime_config().static_file_mounts;
        for (const auto& mount : mounts) {
            if (!path_starts_with_mount(path, mount.prefix)) {
                continue;
            }
            if (best == nullptr || mount.prefix.size() > best->prefix.size()) {
                best = &mount;
            }
        }
        return best;
    }

    static std::string mounted_static_file_path(std::string_view path,
                                             const H2StaticFileMount& mount) {
        if (mount.prefix == "/") {
            return std::string(path);
        }
        std::string relative(path.substr(mount.prefix.size()));
        if (relative.empty()) {
            return "/";
        }
        return relative;
    }

    static uintmax_t static_file_content_length(const H2StaticFileLookup& lookup) {
        for (const auto& header : lookup.headers) {
            if (header.name != "content-length") {
                continue;
            }
            uintmax_t value = 0;
            const auto* begin = header.value.data();
            const auto* end = begin + header.value.size();
            auto [ptr, ec] = std::from_chars(begin, end, value);
            if (ec == std::errc{} && ptr == end) {
                return value;
            }
        }
        return 0;
    }

    enum class H2StaticFileBodyReadError {
        kInvalidRange,
        kOpen,
        kRead,
        kShortRead,
        kClose,
    };

    static std::expected<std::vector<std::string>, H2StaticFileBodyReadError>
    read_static_file_chunks_blocking(const std::string& path,
                                 uintmax_t offset,
                                 uintmax_t length,
                                 uint32_t max_frame_size) {
        if (max_frame_size == 0 ||
            offset > static_cast<uintmax_t>(std::numeric_limits<off_t>::max())) {
            return std::unexpected(H2StaticFileBodyReadError::kInvalidRange);
        }

        std::vector<std::string> chunks;
        const auto frame_size = std::max<uint32_t>(max_frame_size, 1);
        chunks.reserve(static_cast<size_t>((length + frame_size - 1) / frame_size));

        const int fd = ::open(path.c_str(), O_RDONLY);
        if (fd < 0) {
            return std::unexpected(H2StaticFileBodyReadError::kOpen);
        }

        uintmax_t remaining = length;
        uintmax_t current_offset = offset;
        while (remaining > 0) {
            const auto chunk_size = static_cast<size_t>(
                std::min<uintmax_t>(remaining, frame_size));
            std::string chunk(chunk_size, '\0');

            size_t chunk_offset = 0;
            while (chunk_offset < chunk_size) {
                if (current_offset > static_cast<uintmax_t>(std::numeric_limits<off_t>::max())) {
                    const int close_result = ::close(fd);
                    if (close_result != 0) {
                        return std::unexpected(H2StaticFileBodyReadError::kClose);
                    }
                    return std::unexpected(H2StaticFileBodyReadError::kInvalidRange);
                }
                const ssize_t read_count = ::pread(fd,
                                                   chunk.data() + chunk_offset,
                                                   chunk_size - chunk_offset,
                                                   static_cast<off_t>(current_offset));
                if (read_count < 0) {
                    if (errno == EINTR) {
                        continue;
                    }
                    const int close_result = ::close(fd);
                    if (close_result != 0) {
                        return std::unexpected(H2StaticFileBodyReadError::kClose);
                    }
                    return std::unexpected(H2StaticFileBodyReadError::kRead);
                }
                if (read_count == 0) {
                    const int close_result = ::close(fd);
                    if (close_result != 0) {
                        return std::unexpected(H2StaticFileBodyReadError::kClose);
                    }
                    return std::unexpected(H2StaticFileBodyReadError::kShortRead);
                }
                const auto advanced = static_cast<size_t>(read_count);
                chunk_offset += advanced;
                current_offset += static_cast<uintmax_t>(advanced);
            }
            remaining -= static_cast<uintmax_t>(chunk_size);
            chunks.push_back(std::move(chunk));
        }
        const int close_result = ::close(fd);
        if (close_result != 0) {
            return std::unexpected(H2StaticFileBodyReadError::kClose);
        }
        return chunks;
    }

    bool can_send_static_file_body_now(uintmax_t length) const {
        if (length == 0) {
            return true;
        }
        const auto max_body = std::min<int64_t>(
            m_conn.conn_send_window(),
            static_cast<int64_t>(m_conn.peer_settings().initial_window_size));
        return max_body >= 0 && length <= static_cast<uintmax_t>(max_body);
    }

    void append_static_file_data_frames(uint32_t stream_id,
                                    std::vector<std::string>&& chunks) {
        uintmax_t total = 0;
        for (size_t i = 0; i < chunks.size(); ++i) {
            auto& chunk = chunks[i];
            const bool end_stream = i + 1 == chunks.size();
            total += chunk.size();
            m_static_response_batch.push_back(
                Http2OutgoingFrame{Http2FrameBuilder::data_bytes(
                    stream_id, chunk, end_stream)});
        }
        if (total > 0) {
            m_conn.adjust_conn_send_window(-static_cast<int32_t>(total));
        }
    }

    void append_static_file_shared_data_frames(uint32_t stream_id,
                                          std::shared_ptr<const std::string> body) {
        if (!body || body->empty()) {
            return;
        }

        uintmax_t total = 0;
        const auto frame_size = std::max<uint32_t>(m_conn.peer_settings().max_frame_size, 1);
        size_t offset = 0;
        while (offset < body->size()) {
            const auto chunk_size = std::min<size_t>(body->size() - offset, frame_size);
            const bool end_stream = offset + chunk_size == body->size();
            auto data_header = Http2FrameBuilder::data_header_bytes(
                stream_id, chunk_size, end_stream);
            total += chunk_size;
            m_static_response_batch.push_back(
                Http2OutgoingFrame::segmented_shared(
                    std::move(data_header), body, offset, chunk_size));
            offset += chunk_size;
        }
        m_conn.adjust_conn_send_window(-static_cast<int32_t>(total));
    }

    static bool enqueue_static_file_read_failure(galay::mpsc::UnboundedChannel<Http2OutgoingFrame>* send_channel,
                                             uint32_t stream_id) {
        if (send_channel == nullptr) {
            return false;
        }
        auto rst = std::make_unique<Http2RstStreamFrame>();
        rst->header().stream_id = stream_id;
        rst->set_error_code(Http2ErrorCode::InternalError);
        std::vector<Http2OutgoingFrame> frames;
        frames.reserve(1);
        frames.push_back(Http2OutgoingFrame{std::move(rst)});
        return send_channel->send_batch(std::move(frames));
    }

    bool schedule_static_file_body_read(uint32_t stream_id,
                                    std::shared_ptr<const std::string> header_block,
                                    std::shared_ptr<H2StaticFileBodyCacheSlot> body_cache_slot,
                                    std::string file_path,
                                    uintmax_t offset,
                                    uintmax_t length) {
        if (!header_block || length == 0 ||
            length > static_cast<uintmax_t>(std::numeric_limits<int32_t>::max())) {
            return false;
        }
        auto runtime = RuntimeHandle::try_current();
        if (!runtime.has_value()) {
            return false;
        }

        const uint32_t frame_size = std::max<uint32_t>(m_conn.peer_settings().max_frame_size, 1);
        auto header_bytes = Http2FrameBuilder::headers_header_bytes(
            stream_id, header_block->size(), false, true);
        auto* send_channel = &m_send_channel;

        // Reserve flow-control credit on the IO owner before the worker thread reads.
        // The worker only touches its local buffers and the thread-safe send channel.
        m_conn.adjust_conn_send_window(-static_cast<int32_t>(length));
        auto blocking_task = runtime->spawn_blocking(
            [send_channel,
             stream_id,
             header_block = std::move(header_block),
             body_cache_slot = std::move(body_cache_slot),
             header_bytes,
             file_path = std::move(file_path),
             offset,
             length,
             frame_size]() mutable {
                auto chunks_result = read_static_file_chunks_blocking(file_path, offset, length, frame_size);
                if (!chunks_result.has_value()) {
                    const bool sent = enqueue_static_file_read_failure(send_channel, stream_id);
                    if (!sent) {
                        return;
                    }
                    return;
                }

                auto& chunks = chunks_result.value();
                std::shared_ptr<const std::string> cached_body;
                if (body_cache_slot && offset == 0 &&
                    length <= static_cast<uintmax_t>(std::numeric_limits<size_t>::max())) {
                    auto body = std::make_shared<std::string>();
                    body->reserve(static_cast<size_t>(length));
                    bool append_ok = true;
                    for (const auto& chunk : chunks) {
                        std::string& appended = body->append(chunk);
                        if (&appended != body.get()) {
                            append_ok = false;
                            break;
                        }
                    }
                    if (append_ok && body->size() == static_cast<size_t>(length)) {
                        cached_body = body;
                        const bool stored = body_cache_slot->store_if_empty(cached_body);
                        if (!stored) {
                            cached_body = body_cache_slot->load();
                        }
                    }
                }

                std::vector<Http2OutgoingFrame> frames;
                frames.reserve(chunks.size() + 1);
                frames.push_back(
                    Http2OutgoingFrame::segmented_shared(std::move(header_bytes),
                                                        std::move(header_block)));
                if (cached_body) {
                    size_t body_offset = 0;
                    for (size_t i = 0; i < chunks.size(); ++i) {
                        const bool end_stream = i + 1 == chunks.size();
                        const size_t chunk_size = chunks[i].size();
                        auto data_header = Http2FrameBuilder::data_header_bytes(
                            stream_id, chunk_size, end_stream);
                        frames.push_back(Http2OutgoingFrame::segmented_shared(
                            std::move(data_header), cached_body, body_offset, chunk_size));
                        body_offset += chunk_size;
                    }
                } else {
                    for (size_t i = 0; i < chunks.size(); ++i) {
                        const bool end_stream = i + 1 == chunks.size();
                        auto data_header = Http2FrameBuilder::data_header_bytes(
                            stream_id, chunks[i].size(), end_stream);
                        frames.push_back(Http2OutgoingFrame::segmented(
                            std::move(data_header), std::move(chunks[i])));
                    }
                }
                const bool sent = send_channel->send_batch(std::move(frames));
                if (!sent) {
                    return;
                }
            });
        if (!blocking_task.has_value()) {
            m_conn.adjust_conn_send_window(static_cast<int32_t>(length));
            return false;
        }
        if (!blocking_task->is_valid()) {
            m_conn.adjust_conn_send_window(static_cast<int32_t>(length));
            return false;
        }
        return true;
    }

    bool send_static_file_lookup(uint32_t stream_id,
                              std::string_view method,
                              const H2StaticFileLookup& lookup) {
        const bool is_head = method == "HEAD";
        const uintmax_t length = static_file_content_length(lookup);
        const bool has_body = !is_head && length > 0 &&
            lookup.status != 304 && lookup.status != 416;
        if (has_body && !can_send_static_file_body_now(length)) {
            return false;
        }
        auto header_block = lookup.encoded_headers
            ? lookup.encoded_headers
            : encode_h2_static_file_headers(lookup.status, lookup.headers);
        if (has_body && !lookup.body) {
            const uintmax_t body_offset = lookup.status == 206 ? lookup.range_start : 0;
            return schedule_static_file_body_read(stream_id,
                                              std::move(header_block),
                                              lookup.body_cacheable ? lookup.body_cache_slot : nullptr,
                                              lookup.file_path.string(),
                                              body_offset,
                                              length);
        }

        const bool headers_end_stream = !has_body;
        auto header_bytes = Http2FrameBuilder::headers_header_bytes(
            stream_id, header_block->size(), headers_end_stream, true);
        m_static_response_batch.push_back(
            Http2OutgoingFrame::segmented_shared(std::move(header_bytes), std::move(header_block)));
        if (!has_body) {
            return true;
        }

        if (lookup.body) {
            append_static_file_shared_data_frames(stream_id, lookup.body);
        }
        return true;
    }

    bool send_static_file_fast_lookup(uint32_t stream_id,
                                  std::string_view method,
                                  const H2StaticFileFastLookup& lookup) {
        if (!lookup.encoded_headers) {
            return false;
        }

        const bool is_head = method == "HEAD";
        const uintmax_t length = lookup.content_length;
        const bool has_body = !is_head && length > 0;
        if (has_body && !can_send_static_file_body_now(length)) {
            return false;
        }

        if (has_body && !lookup.body) {
            return schedule_static_file_body_read(stream_id,
                                              lookup.encoded_headers,
                                              lookup.body_cacheable ? lookup.body_cache_slot : nullptr,
                                              lookup.file_path.string(),
                                              0,
                                              length);
        }

        auto header_bytes = Http2FrameBuilder::headers_header_bytes(
            stream_id, lookup.encoded_headers->size(), !has_body, true);
        m_static_response_batch.push_back(
            Http2OutgoingFrame::segmented_shared(
                std::move(header_bytes), lookup.encoded_headers));
        if (!has_body) {
            return true;
        }

        if (lookup.body) {
            append_static_file_shared_data_frames(stream_id, lookup.body);
        }
        return true;
    }

    bool try_send_static_file(uint32_t stream_id,
                           std::string_view method,
                           std::string_view path,
                           std::string_view if_none_match,
                           std::string_view range) {
        if (!is_static_file_method(method)) {
            return false;
        }
        const auto* mount = find_static_file_mount(path);
        if (mount == nullptr || !mount->cache) {
            return false;
        }

        auto mounted_path = mounted_static_file_path(path, *mount);
        if (if_none_match.empty() && range.empty()) {
            auto fast_lookup = mount->cache->lookup_fast200(mounted_path);
            if (fast_lookup.has_value()) {
                return send_static_file_fast_lookup(stream_id, method, *fast_lookup);
            }
        }

        auto lookup = mount->cache->lookup(H2StaticFileRequest{
            .path = std::move(mounted_path),
            .if_none_match = std::string(if_none_match),
            .range = std::string(range),
        });
        return send_static_file_lookup(stream_id, method, lookup);
    }

    bool try_send_static_file(uint32_t stream_id,
                           const std::vector<Http2HeaderField>& fields) {
        std::string_view method;
        std::string_view path;
        std::string_view if_none_match;
        std::string_view range;
        for (const auto& field : fields) {
            if (field.name == ":method") {
                method = field.value;
            } else if (field.name == ":path") {
                path = field.value;
            } else if (field.name == "if-none-match") {
                if_none_match = field.value;
            } else if (field.name == "range") {
                range = field.value;
            }
        }
        if (method.empty() || path.empty()) {
            return false;
        }
        return try_send_static_file(stream_id, method, path, if_none_match, range);
    }

    bool try_send_static_file(const Http2Stream::ptr& stream) {
        if (m_conn.is_client() || !stream || !stream->is_request_completed()) {
            return false;
        }
        const auto& request = stream->request();
        return try_send_static_file(stream->stream_id(),
                                 request.method,
                                 request.path,
                                 request.get_header("if-none-match"),
                                 request.get_header("range"));
    }

    bool can_send_static_body_now(const H2StaticResponse& response) const {
        if (response.body.empty()) {
            return true;
        }
        return response.body.size() <= m_conn.peer_settings().max_frame_size &&
               response.body.size() <= m_conn.peer_settings().initial_window_size;
    }

    bool try_send_static_response(uint32_t stream_id,
                               std::string_view method,
                               std::string_view path) {
        const auto* route = find_static_response_route(method, path);
        if (route == nullptr) {
            return false;
        }
        if (!can_send_static_body_now(route->response)) {
            return false;
        }

        auto payload = route->encoded_headers
            ? route->encoded_headers
            : encode_h2_static_response_headers(route->response);
        const bool is_head = method == "HEAD";
        const bool headers_end_stream = is_head || route->response.body.empty();
        auto header_bytes = Http2FrameBuilder::headers_header_bytes(
            stream_id, payload->size(), headers_end_stream, true);
        m_static_response_batch.push_back(
            Http2OutgoingFrame::segmented_shared(std::move(header_bytes), std::move(payload)));
        if (!headers_end_stream) {
            auto body = route->shared_body
                ? route->shared_body
                : std::make_shared<const std::string>(route->response.body);
            auto data_header = Http2FrameBuilder::data_header_bytes(
                stream_id, body->size(), true);
            m_static_response_batch.push_back(
                Http2OutgoingFrame::segmented_shared(
                    std::move(data_header), std::move(body)));
        }
        return true;
    }

    bool try_send_static_response(uint32_t stream_id,
                               const std::vector<Http2HeaderField>& fields) {
        std::string_view method;
        std::string_view path;
        for (const auto& field : fields) {
            if (field.name == ":method") {
                method = field.value;
            } else if (field.name == ":path") {
                path = field.value;
            }
        }
        if (method.empty() || path.empty()) {
            return false;
        }
        return try_send_static_response(stream_id, method, path);
    }

    bool try_send_static_response(const Http2Stream::ptr& stream) {
        if (m_conn.is_client() || !stream || !stream->is_request_completed()) {
            return false;
        }

        const auto& request = stream->request();
        const auto* route = find_static_response_route(request.method, request.path);
        if (route == nullptr) {
            return false;
        }
        if (!can_send_static_body_now(route->response)) {
            return false;
        }

        auto header_block = route->encoded_headers
            ? route->encoded_headers
            : encode_h2_static_response_headers(route->response);
        const bool headers_end_stream =
            request.method == "HEAD" || route->response.body.empty();
        if (headers_end_stream) {
            stream->send_encoded_headers(std::move(header_block), true, true);
        } else {
            stream->send_encoded_headers_and_data(
                std::move(header_block), route->response.body, true);
        }
        return true;
    }

    void complete_decoded_headers(const Http2Stream::ptr& stream, bool end_stream) {
        const bool initial_headers = !stream->is_end_headers_received();
        stream->set_end_headers_received();
        if (m_conn.is_client()) {
            stream->consume_decoded_headers_as_response();
            auto events = Http2StreamEvent::HeadersReady;
            if (end_stream) {
                stream->mark_response_completed();
                events |= Http2StreamEvent::ResponseComplete;
            }
            mark_stream_active(stream, events);
            return;
        }

        stream->consume_decoded_headers_as_request();
        auto events = Http2StreamEvent::HeadersReady;
        if (end_stream) {
            stream->mark_request_completed();
            events |= Http2StreamEvent::RequestComplete;
        }
        if (try_send_static_response(stream)) {
            return;
        }
        if (try_send_static_file(stream)) {
            return;
        }
        if (m_active_conn_mode) {
            if (should_defer_headers_only_active_delivery(stream, end_stream)) {
                stream->m_pending_events |= events;
                return;
            }
            mark_stream_active(stream, events);
        } else if (initial_headers) {
            queue_stream_handler(stream);
        }
    }

    void complete_received_headers(const Http2Stream::ptr& stream, bool end_stream) {
        decode_buffered_headers(stream);
        complete_decoded_headers(stream, end_stream);
    }

    void apply_recv_window_update(const Http2Stream::ptr& stream, uint32_t stream_id, size_t data_size) {
        auto update = m_conn.evaluate_recv_window_update(stream->recv_window(), data_size);
        if (update.conn_increment > 0) {
            enqueue_window_update_action(0, update.conn_increment);
            m_conn.adjust_conn_recv_window(static_cast<int32_t>(update.conn_increment));
        }
        if (update.stream_increment > 0) {
            enqueue_window_update_action(stream_id, update.stream_increment);
            stream->adjust_recv_window(static_cast<int32_t>(update.stream_increment));
        }
    }

    void append_stream_data_and_mark_events(const Http2Stream::ptr& stream, Http2DataFrame* data) {
        auto events = Http2StreamEvent::DataArrived;
        if (m_conn.is_client()) {
            stream->append_response_data(data->data());
            if (data->is_end_stream()) {
                stream->mark_response_completed();
                events |= Http2StreamEvent::ResponseComplete;
            }
        } else {
            if (m_active_conn_mode) {
                stream->append_request_data(std::move(data->data()));
            } else {
                stream->append_request_data(data->data());
            }
            if (data->is_end_stream()) {
                stream->mark_request_completed();
                events |= Http2StreamEvent::RequestComplete;
            }
        }
        mark_stream_active(stream, events);
    }

    void append_stream_data_and_mark_events(const Http2Stream::ptr& stream,
                                       std::string_view data,
                                       bool end_stream) {
        auto events = Http2StreamEvent::DataArrived;
        if (m_conn.is_client()) {
            stream->append_response_data(std::string(data));
            if (end_stream) {
                stream->mark_response_completed();
                events |= Http2StreamEvent::ResponseComplete;
            }
        } else {
            stream->append_request_data(data);
            if (end_stream) {
                stream->mark_request_completed();
                events |= Http2StreamEvent::RequestComplete;
            }
        }
        mark_stream_active(stream, events);
    }

    std::expected<Http2Frame::uptr, Http2ErrorCode> materialize_frame_view(
        const Http2RawFrameView& frame_view) {
        auto bytes = frame_view.bytes();
        return Http2FrameParser::parse_frame(
            reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
    }

    bool handle_raw_headers_frame_view(const Http2RawFrameView& frame_view, uint32_t stream_id) {
        if ((frame_view.header.flags & Http2FrameFlags::kPadded) != 0 ||
            (frame_view.header.flags & Http2FrameFlags::kPriority) != 0) {
            return false;
        }

        auto stream = find_attached_stream(stream_id);
        if (!stream) {
            if (m_reject_new_streams ||
                (m_conn.is_goaway_sent() && m_conn.goaway_last_stream_id() != kMaxStreamId)) {
                enqueue_rst_stream_action(stream_id, Http2ErrorCode::RefusedStream);
                return true;
            }
            if (stream_id <= m_conn.last_peer_stream_id()) {
                enqueue_goaway_action(Http2ErrorCode::ProtocolError);
                return true;
            }
            if (m_conn.stream_count() >= m_conn.local_settings().max_concurrent_streams) {
                enqueue_rst_stream_action(stream_id, Http2ErrorCode::RefusedStream);
                return true;
            }

            const bool end_headers = frame_view.end_headers();
            const bool end_stream = frame_view.end_stream();
            if (end_headers) {
                auto payload = frame_view.payload();
                if (end_stream && !m_conn.runtime_config().static_file_mounts.empty()) {
                    auto decoder_snapshot = m_conn.decoder().clone();
                    auto target = m_conn.decoder().decode_request_target(
                        reinterpret_cast<const uint8_t*>(payload.data()), payload.size());
                    if (!target) {
                        enqueue_goaway_action(target.error());
                        return true;
                    }
                    if (try_send_static_response(stream_id, target->method, target->path) ||
                        try_send_static_file(stream_id,
                                          target->method,
                                          target->path,
                                          target->if_none_match,
                                          target->range)) {
                        m_conn.set_last_peer_stream_id(stream_id);
                        return true;
                    }
                    m_conn.decoder() = std::move(decoder_snapshot);

                    auto fields = m_conn.decoder().decode(
                        reinterpret_cast<const uint8_t*>(payload.data()), payload.size());
                    if (!fields) {
                        enqueue_goaway_action(fields.error());
                        return true;
                    }
                    stream = create_stream_internal(stream_id);
                    m_conn.set_last_peer_stream_id(stream_id);
                    stream->on_headers_received(end_stream);
                    stream->set_decoded_headers(std::move(*fields));
                    complete_decoded_headers(stream, end_stream);
                    try_retire_client_stream(stream);
                    return true;
                }
                if (end_stream && !m_conn.runtime_config().static_routes.empty()) {
                    auto decoder_snapshot = m_conn.decoder().clone();
                    auto target = m_conn.decoder().decode_request_target(
                        reinterpret_cast<const uint8_t*>(payload.data()), payload.size());
                    if (!target) {
                        enqueue_goaway_action(target.error());
                        return true;
                    }
                    if (try_send_static_response(stream_id, target->method, target->path)) {
                        m_conn.set_last_peer_stream_id(stream_id);
                        return true;
                    }
                    m_conn.decoder() = std::move(decoder_snapshot);
                }

                auto fields = m_conn.decoder().decode(
                    reinterpret_cast<const uint8_t*>(payload.data()), payload.size());
                if (!fields) {
                    enqueue_goaway_action(fields.error());
                    return true;
                }

                stream = create_stream_internal(stream_id);
                m_conn.set_last_peer_stream_id(stream_id);
                stream->on_headers_received(end_stream);
                stream->set_decoded_headers(std::move(*fields));
                complete_decoded_headers(stream, end_stream);
                try_retire_client_stream(stream);
                return true;
            }

            stream = create_stream_internal(stream_id);
            m_conn.set_last_peer_stream_id(stream_id);
        }
        if (!stream) {
            return true;
        }

        if (!stream->can_receive_headers()) {
            enqueue_rst_stream_action(stream_id, Http2ErrorCode::StreamClosed);
            return true;
        }

        const bool end_headers = frame_view.end_headers();
        const bool end_stream = frame_view.end_stream();
        stream->on_headers_received(end_stream);
        stream->append_header_block(frame_view.payload());

        if (end_headers) {
            complete_received_headers(stream, end_stream);
        } else {
            m_conn.set_expecting_continuation(true, stream_id);
        }

        try_retire_client_stream(stream);
        return true;
    }

    bool handle_raw_continuation_frame_view(const Http2RawFrameView& frame_view, uint32_t stream_id) {
        auto stream = find_attached_stream(stream_id);
        if (!stream) {
            enqueue_goaway_action(Http2ErrorCode::ProtocolError);
            return true;
        }

        stream->append_header_block(frame_view.payload());

        if (frame_view.end_headers()) {
            complete_received_headers(stream, stream->is_end_stream_received());
        }

        return true;
    }

    bool handle_raw_data_frame_view(const Http2RawFrameView& frame_view, uint32_t stream_id) {
        if ((frame_view.header.flags & Http2FrameFlags::kPadded) != 0) {
            return false;
        }

        if (stream_id == 0) {
            enqueue_goaway_action(Http2ErrorCode::ProtocolError);
            return true;
        }

        auto stream = find_attached_stream(stream_id);
        if (!stream) {
            enqueue_rst_stream_action(stream_id, Http2ErrorCode::StreamClosed);
            return true;
        }
        if (!stream->can_receive_data()) {
            enqueue_rst_stream_action(stream_id, Http2ErrorCode::StreamClosed);
            return true;
        }

        const auto payload = frame_view.payload();
        const size_t data_size = payload.size();
        if (data_size > static_cast<size_t>(std::max<int32_t>(m_conn.conn_recv_window(), 0))) {
            enqueue_goaway_action(Http2ErrorCode::FlowControlError);
            return true;
        }
        if (data_size > static_cast<size_t>(std::max<int32_t>(stream->recv_window(), 0))) {
            enqueue_rst_stream_action(stream_id, Http2ErrorCode::FlowControlError);
            return true;
        }

        const int32_t data_size_delta = static_cast<int32_t>(data_size);
        stream->on_data_received(frame_view.end_stream());

        m_conn.adjust_conn_recv_window(-data_size_delta);
        stream->adjust_recv_window(-data_size_delta);
        apply_recv_window_update(stream, stream_id, data_size);
        append_stream_data_and_mark_events(stream, payload, frame_view.end_stream());

        try_retire_client_stream(stream);
        return true;
    }

    bool try_dispatch_server_active_frame_view(Http2RawFrameView&& frame_view) {
        if (!m_active_conn_mode || m_conn.is_client()) {
            return false;
        }

        const uint32_t stream_id = frame_view.stream_id();

        if (frame_view.is_headers()) {
            return handle_raw_headers_frame_view(frame_view, stream_id);
        }

        if (frame_view.is_continuation()) {
            return handle_raw_continuation_frame_view(frame_view, stream_id);
        }

        if (frame_view.is_data()) {
            return handle_raw_data_frame_view(frame_view, stream_id);
        }

        return false;
    }

    void handle_headers_frame(Http2Frame::uptr frame, uint32_t stream_id) {
        auto stream = find_or_create_headers_stream(stream_id);
        if (!stream) {
            return;
        }

        if (!stream->can_receive_headers()) {
            enqueue_rst_stream_action(stream_id, Http2ErrorCode::StreamClosed);
            return;
        }

        auto* hdrs = frame->as_headers();
        if (hdrs->has_priority()) {
            stream->set_priority(hdrs->exclusive(), hdrs->stream_dependency(), hdrs->weight());
        }

        const bool end_headers = hdrs->is_end_headers();
        const bool end_stream = hdrs->is_end_stream();
        stream->on_headers_received(end_stream);
        stream->append_header_block(hdrs->header_block());

        if (end_headers) {
            complete_received_headers(stream, end_stream);
        } else {
            m_conn.set_expecting_continuation(true, stream_id);
        }

        push_stream_frame_if_needed(stream, std::move(frame));
        try_retire_client_stream(stream);
    }

    void handle_continuation_frame(Http2Frame::uptr frame, uint32_t stream_id) {
        auto stream = find_attached_stream(stream_id);
        if (!stream) {
            enqueue_goaway_action(Http2ErrorCode::ProtocolError);
            return;
        }

        auto* cont = frame->as_continuation();
        stream->append_header_block(cont->header_block());

        if (cont->is_end_headers()) {
            complete_received_headers(stream, stream->is_end_stream_received());
        }

        push_stream_frame_if_needed(stream, std::move(frame));
    }

    void handle_data_frame(Http2Frame::uptr frame, uint32_t stream_id) {
        if (stream_id == 0) {
            enqueue_goaway_action(Http2ErrorCode::ProtocolError);
            return;
        }

        auto stream = find_attached_stream(stream_id);
        if (!stream) {
            enqueue_rst_stream_action(stream_id, Http2ErrorCode::StreamClosed);
            return;
        }
        if (!stream->can_receive_data()) {
            enqueue_rst_stream_action(stream_id, Http2ErrorCode::StreamClosed);
            return;
        }

        auto* data = frame->as_data();
        const size_t data_size = data->data().size();
        if (data_size > static_cast<size_t>(std::max<int32_t>(m_conn.conn_recv_window(), 0))) {
            enqueue_goaway_action(Http2ErrorCode::FlowControlError);
            return;
        }
        if (data_size > static_cast<size_t>(std::max<int32_t>(stream->recv_window(), 0))) {
            enqueue_rst_stream_action(stream_id, Http2ErrorCode::FlowControlError);
            return;
        }

        const int32_t data_size_delta = static_cast<int32_t>(data_size);
        stream->on_data_received(data->is_end_stream());

        m_conn.adjust_conn_recv_window(-data_size_delta);
        stream->adjust_recv_window(-data_size_delta);
        apply_recv_window_update(stream, stream_id, data_size);
        append_stream_data_and_mark_events(stream, data);

        push_stream_frame_if_needed(stream, std::move(frame));
        try_retire_client_stream(stream);
    }

    void handle_priority_frame(Http2Frame::uptr frame, uint32_t stream_id) {
        auto stream = find_attached_stream(stream_id);
        if (!stream) {
            return;
        }

        auto* prio = frame->as_priority();
        stream->set_priority(prio->exclusive(), prio->stream_dependency(), prio->weight());
    }

    void handle_rst_stream_frame(Http2Frame::uptr frame, uint32_t stream_id) {
        if (stream_id == 0) {
            enqueue_goaway_action(Http2ErrorCode::ProtocolError);
            return;
        }

        auto stream = find_attached_stream(stream_id);
        if (!stream) {
            return;
        }

        stream->on_rst_stream_received();
        mark_stream_active(stream, Http2StreamEvent::Reset);
        push_stream_frame_if_needed(stream, std::move(frame));
        stream->mark_request_completed();
        stream->mark_response_completed();
        stream->close_frame_queue();
        try_retire_client_stream(stream);
    }

    void handle_window_update_frame(Http2Frame::uptr frame, uint32_t stream_id) {
        auto stream = find_attached_stream(stream_id);
        if (!stream) {
            return;
        }

        auto* wu = frame->as_window_update();
        const uint32_t increment = wu->window_size_increment();
        if (increment == 0) {
            enqueue_rst_stream_action(stream_id, Http2ErrorCode::ProtocolError);
            return;
        }
        if (static_cast<int64_t>(stream->send_window()) + increment > kMaxStreamId) {
            enqueue_rst_stream_action(stream_id, Http2ErrorCode::FlowControlError);
            return;
        }

        stream->adjust_send_window(increment);
        stream->m_max_frame_size = m_conn.peer_settings().max_frame_size;
        stream->m_max_header_list_size = m_conn.peer_settings().max_header_list_size;
        const bool made_progress = stream->flush_pending_data();
        // made_progress only reports whether queued DATA was flushed now.
        mark_stream_active(stream, Http2StreamEvent::WindowUpdated);
        push_stream_frame_if_needed(stream, std::move(frame));
    }

    void handle_push_promise_frame(Http2Frame::uptr frame, uint32_t stream_id) {
        if (!m_conn.is_client()) {
            enqueue_goaway_action(Http2ErrorCode::ProtocolError);
            return;
        }

        auto* pp = frame->as_push_promise();
        const uint32_t promised_id = pp->promised_stream_id();
        auto promised_stream = find_attached_stream(promised_id);
        if (!promised_stream) {
            promised_stream = create_stream_internal(promised_id);
            promised_stream->set_state(Http2StreamState::ReservedRemote);
        }

        push_stream_frame_if_needed(promised_stream, std::move(frame));
        if (!m_active_conn_mode) {
            queue_stream_handler(promised_stream);
        }
    }

    void dispatch_stream_frame(Http2Frame::uptr frame) {
        const uint32_t stream_id = frame->stream_id();

        if (stream_id == 0) {
            return;
        }

        if (frame->is_headers()) {
            handle_headers_frame(std::move(frame), stream_id);
            return;
        }

        if (frame->is_continuation()) {
            handle_continuation_frame(std::move(frame), stream_id);
            return;
        }

        if (frame->is_data()) {
            handle_data_frame(std::move(frame), stream_id);
            return;
        }

        if (frame->is_priority()) {
            handle_priority_frame(std::move(frame), stream_id);
            return;
        }

        if (frame->is_rst_stream()) {
            handle_rst_stream_frame(std::move(frame), stream_id);
            return;
        }

        if (frame->is_window_update()) {
            handle_window_update_frame(std::move(frame), stream_id);
            return;
        }

        if (frame->is_push_promise()) {
            handle_push_promise_frame(std::move(frame), stream_id);
            return;
        }

    }

    /**
     * @brief 处理 dispatch_stream_frame 中标记的待处理动作（通过 channel 发送）
     */
    void process_pending_actions() {
        while (!m_pending_actions.empty()) {
            auto action = m_pending_actions.front();
            m_pending_actions.pop_front();

            switch (action.type) {
                case PendingAction::Type::SendGoaway: {
                    enqueue_goaway(action.error_code);
                    break;
                }
                case PendingAction::Type::SendRstStream: {
                    auto bytes = Http2FrameBuilder::rst_stream_bytes(action.stream_id, action.error_code);
                    auto stream = m_conn.get_stream(action.stream_id);
                    if (stream) {
                        stream->on_rst_stream_sent();
                    }
                    enqueue_send_bytes(std::move(bytes));
                    break;
                }
                case PendingAction::Type::SendWindowUpdate: {
                    Http2WindowUpdateFrame frame;
                    frame.header().stream_id = action.stream_id;
                    frame.set_window_size_increment(action.increment);
                    enqueue_send_frame(std::move(frame));
                    break;
                }
            }
        }
    }

    void flush_static_response_batch() {
        if (m_static_response_batch.empty()) {
            return;
        }
        enqueue_outgoing_batch(std::move(m_static_response_batch));
        m_static_response_batch.clear();
    }

    /**
     * @brief 入队 GOAWAY 帧
     */
    void enqueue_goaway(Http2ErrorCode error,
                       const std::string& debug = "",
                       const Http2OutgoingFrame::WaiterPtr& waiter = nullptr,
                       std::optional<uint32_t> last_stream_id = std::nullopt) {
        Http2GoAwayFrame frame;
        uint32_t last = last_stream_id.value_or(m_conn.last_peer_stream_id());
        frame.set_last_stream_id(last);
        frame.set_error_code(error);
        if (!debug.empty()) {
            frame.set_debug_data(debug);
        }
        m_conn.mark_goaway_sent(last, error, debug);
        enqueue_send_frame(std::move(frame), waiter);
    }

    /**
     * @brief 将新流加入待 spawn 队列
     */
    void queue_stream_handler(Http2Stream::ptr stream) {
        m_pending_spawns.push(stream);
    }

    void mark_stream_active(const Http2Stream::ptr& stream, Http2StreamEvent events) {
        if (!m_active_conn_mode) {
            return;
        }
        m_active_batch.mark(stream, events);
    }

    bool should_defer_headers_only_active_delivery(const Http2Stream::ptr& stream,
                                              bool end_stream) const {
        if (!stream || !m_active_conn_mode || m_conn.is_client() || end_stream) {
            return false;
        }

        const auto content_length = stream->request().get_header("content-length");
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

    void close_active_stream_queue() {
        if (!m_active_conn_mode || m_active_stream_queue_closed) {
            return;
        }
        m_active_stream_queue_closed = true;
        m_active_stream_mailbox.close();
    }

    void push_stream_frame_if_needed(const Http2Stream::ptr& stream, Http2Frame::uptr frame) {
        if (!stream) {
            return;
        }
        if (m_active_conn_mode) {
            return;
        }
        stream->push_frame(std::move(frame));
    }

    void flush_active_streams() {
        if (!m_active_conn_mode || m_active_batch.empty()) {
            return;
        }

        auto ready = m_active_batch.take_ready();
        m_active_stream_mailbox.send_batch(std::move(ready));
    }

    void try_retire_client_stream(const Http2Stream::ptr& stream) {
        if (!stream || !m_conn.is_client()) {
            return;
        }
        if (!stream->is_response_completed()) {
            return;
        }
        if (stream->state() != Http2StreamState::Closed) {
            return;
        }
        m_conn.remove_stream(stream->stream_id());
    }

    Http2Stream::ptr create_stream_internal(uint32_t stream_id) {
        drain_retired_streams();
        Http2Stream::ptr stream;
        if (m_active_conn_mode && !m_conn.is_client()) {
            stream = m_conn.create_stream(stream_id, m_stream_pool.acquire(stream_id));
        } else {
            stream = m_conn.create_stream(stream_id);
        }
        attach_stream_io(stream);
        remember_hot_stream(stream);
        return stream;
    }

    void attach_stream_io(const Http2Stream::ptr& stream) {
        if (!stream) return;
        auto* encoder = &m_conn.encoder();
        auto* decoder = &m_conn.decoder();
        if (stream->m_io_attached &&
            stream->m_send_channel == &m_send_channel &&
            stream->m_encoder == encoder &&
            stream->m_decoder == decoder) {
            return;
        }
        stream->attach_io(&m_send_channel,
                         encoder,
                         decoder,
                         &m_conn.m_conn_send_window,
                         m_conn.peer_settings().max_frame_size,
                         m_conn.peer_settings().max_header_list_size);
        if (m_active_conn_mode && !m_conn.is_client()) {
            stream->set_retire_callback([this](uint32_t stream_id) {
                enqueue_retire_stream(stream_id);
            });
        } else {
            stream->set_retire_callback(nullptr);
        }
    }

    void remember_hot_stream(const Http2Stream::ptr& stream) {
        if (!stream || !m_active_conn_mode || m_conn.is_client()) {
            return;
        }
        m_hot_stream = stream;
    }

    void clear_hot_stream(uint32_t stream_id) {
        if (m_hot_stream && m_hot_stream->stream_id() == stream_id) {
            m_hot_stream.reset();
        }
    }

    Http2ConnImpl<SocketType, Strategy>& m_conn;
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
    galay::mpsc::UnboundedChannel<Http2OutgoingFrame> m_send_channel;
    std::vector<Http2OutgoingFrame> m_static_response_batch;
    std::atomic<bool> m_send_channel_failed{false};

    // 待处理动作队列
    std::deque<PendingAction> m_pending_actions;

    // 待 spawn 的流队列（按优先级排序）
    std::priority_queue<Http2Stream::ptr, std::vector<Http2Stream::ptr>, StreamPriorityCompare> m_pending_spawns;
    galay::mpsc::UnboundedChannel<uint32_t> m_retire_stream_channel;
};

// 类型别名
using Http2StreamManager = Http2StreamManagerImpl<galay::async::AsyncTcpSocket>;

} // namespace galay::http2

#endif // GALAY_HTTP2_STREAM_MANAGER_H
