/**
 * @file h2_core.h
 * @brief HTTP/2 连接核心状态管理
 * @author galay-http
 * @version 1.0.0
 *
 * @details 提供 HTTP/2 连接级别的核心状态管理，包括定时器配置、
 *          连接生命周期控制和协议状态转换。
 */

#ifndef GALAY_HTTP2_CONNECTION_CORE_H
#define GALAY_HTTP2_CONNECTION_CORE_H

#include "frame_disp.h"
#include "out_sched.h"
#include "../../galay-kernel/core/task.h"
#include <atomic>
#include <chrono>
#include <string>

namespace galay::http2
{

/**
 * @brief HTTP/2 连接级协议核心（重写阶段骨架）
 * @details 作为后续 connection-loop + frame-dispatch 的唯一承载对象。
 */
class Http2ConnectionCore
{
public:
    /**
     * @brief 定时器配置
     */
    struct TimerConfig {
        std::chrono::milliseconds settings_ack_timeout{10000};   ///< SETTINGS ACK 超时时间
        std::chrono::milliseconds ping_interval{30000};          ///< PING 发送间隔
        std::chrono::milliseconds ping_timeout{10000};           ///< PING ACK 超时时间
        std::chrono::milliseconds graceful_shutdown_timeout{5000}; ///< 优雅关闭超时时间
    };

    /**
     * @brief 定时器事件类型
     */
    enum class TimerEvent {
        None,                       ///< 无事件
        SendPing,                   ///< 发送 PING
        SettingsAckTimeout,         ///< SETTINGS ACK 超时
        PingAckTimeout,             ///< PING ACK 超时
        GracefulShutdownTimeout     ///< 优雅关闭超时
    };

    /**
     * @brief 连接状态
     */
    enum class State {
        Idle,       ///< 空闲
        Running,    ///< 运行中
        Draining,   ///< 优雅关闭中，不再接受新流
        Closing,    ///< 强制关闭中
        Stopped     ///< 已停止
    };

    Http2ConnectionCore() = default;

    State state() const noexcept { return m_state.load(std::memory_order_acquire); }
    bool stopRequested() const noexcept { return m_stop_requested.load(std::memory_order_acquire); }
    bool outboundReady() const noexcept { return m_outbound_ready; }
    bool hasOutboundWork() const noexcept;
    bool acceptsNewStreams() const noexcept;

    void requestStop() noexcept { m_stop_requested.store(true, std::memory_order_release); }
    void forceClose() noexcept {
        m_state.store(State::Closing, std::memory_order_release);
        requestStop();
    }

    void setTimerConfig(TimerConfig cfg) noexcept { m_timer_config = cfg; }
    const TimerConfig& timerConfig() const noexcept { return m_timer_config; }

    void markSettingsSent() noexcept {
        markSettingsSent(std::chrono::steady_clock::now());
    }
    void markSettingsSent(std::chrono::steady_clock::time_point at) noexcept {
        m_settings_ack_pending.store(true, std::memory_order_release);
        m_settings_sent_at = at;
    }
    void markSettingsAcked() noexcept { m_settings_ack_pending.store(false, std::memory_order_release); }
    bool isSettingsAckPending() const noexcept { return m_settings_ack_pending.load(std::memory_order_acquire); }

    void markFrameReceivedAt(std::chrono::steady_clock::time_point at) noexcept { m_last_frame_recv_at = at; }
    void markPingSent(std::chrono::steady_clock::time_point at) noexcept {
        m_waiting_ping_ack = true;
        m_last_ping_sent_at = at;
    }
    void markPingAcked() noexcept { m_waiting_ping_ack = false; }
    bool waitingPingAck() const noexcept { return m_waiting_ping_ack; }

    void beginGracefulShutdown(std::chrono::steady_clock::time_point at) noexcept {
        m_graceful_shutdown_started = true;
        m_graceful_shutdown_started_at = at;
        m_state.store(State::Draining, std::memory_order_release);
    }

    TimerEvent checkTimers(std::chrono::steady_clock::time_point now) noexcept;
    void applyTimerEvent(TimerEvent event) noexcept;

    /**
     * @brief 处理一帧入站 HTTP/2 帧
     * @details 调用 frame dispatcher 并将产生的控制动作写入出站队列；不执行 I/O，不阻塞。
     * @param frame 入站帧，调用方负责保证其生命周期覆盖本次调用
     * @return dispatcher 结果，错误通过作用域和 action 表达
     */
    H2DispatchResult receiveFrame(const Http2Frame& frame);

    /**
     * @brief 入队待发送 DATA
     * @param stream_id stream ID
     * @param data DATA payload
     * @param end_stream 数据发送完后是否附带 END_STREAM
     * @param weight stream 调度权重
     */
    void enqueueData(uint32_t stream_id, std::string data, bool end_stream, uint8_t weight = 16);

    /**
     * @brief 立即调度当前出站队列
     * @details control/headers 立即出队，DATA 受 budget 和 DRR 限制；不执行 I/O。
     * @param budget 本次发送预算
     * @param config DRR 调度配置
     * @return 本次选出的待发送帧
     */
    H2OutboundSelection flushOutbound(H2OutboundBudget budget, H2SchedulerConfig config = {});

    /**
     * @brief 立即调度当前出站队列并返回已序列化帧 bytes
     * @details control/headers 从现有帧对象序列化，DATA 走 bytes 调度热路径；
     *          不执行 I/O，不阻塞，适合生产写路径减少 DATA frame 对象分配。
     * @param budget 本次发送预算
     * @param config DRR 调度配置
     * @return 本次选出的已序列化帧 bytes
     */
    H2OutboundBytesSelection flushOutboundBytes(H2OutboundBudget budget, H2SchedulerConfig config = {});

    galay::kernel::Task<void> run();

private:
    void enqueueDispatchAction(const H2DispatchAction& action);

    std::atomic<State> m_state{State::Idle};
    std::atomic<bool> m_stop_requested{false};
    std::atomic<bool> m_settings_ack_pending{false};
    TimerConfig m_timer_config{};
    std::chrono::steady_clock::time_point m_settings_sent_at{};
    std::chrono::steady_clock::time_point m_last_frame_recv_at{std::chrono::steady_clock::now()};
    std::chrono::steady_clock::time_point m_last_ping_sent_at{};
    bool m_waiting_ping_ack = false;
    bool m_graceful_shutdown_started = false;
    std::chrono::steady_clock::time_point m_graceful_shutdown_started_at{};
    H2DispatcherConnectionState m_dispatch_state;
    H2OutboundQueues m_outbound_queues;
    bool m_outbound_ready = false;
};

} // namespace galay::http2

#endif // GALAY_HTTP2_CONNECTION_CORE_H
