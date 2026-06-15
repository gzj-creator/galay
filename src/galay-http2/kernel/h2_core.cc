#include "h2_core.h"
#include "frame_disp.h"
#include "out_sched.h"
#include "galay-kernel/common/sleep.hpp"

namespace galay::http2
{

Http2ConnectionCore::TimerEvent Http2ConnectionCore::checkTimers(std::chrono::steady_clock::time_point now) noexcept
{
    if (m_settings_ack_pending.load(std::memory_order_acquire) &&
        m_timer_config.settings_ack_timeout.count() > 0 &&
        now - m_settings_sent_at > m_timer_config.settings_ack_timeout) {
        return TimerEvent::SettingsAckTimeout;
    }

    if (m_graceful_shutdown_started &&
        m_timer_config.graceful_shutdown_timeout.count() > 0 &&
        now - m_graceful_shutdown_started_at > m_timer_config.graceful_shutdown_timeout) {
        return TimerEvent::GracefulShutdownTimeout;
    }

    if (!m_waiting_ping_ack) {
        if (m_timer_config.ping_interval.count() > 0 &&
            now - m_last_frame_recv_at >= m_timer_config.ping_interval) {
            m_waiting_ping_ack = true;
            m_last_ping_sent_at = now;
            return TimerEvent::SendPing;
        }
    } else if (m_timer_config.ping_timeout.count() > 0 &&
               now - m_last_ping_sent_at > m_timer_config.ping_timeout) {
        return TimerEvent::PingAckTimeout;
    }

    return TimerEvent::None;
}

galay::kernel::Task<void> Http2ConnectionCore::run()
{
    H2DispatcherConnectionState dispatch_state;
    H2OutboundBudget budget;
    std::vector<H2StreamSendState> pending_streams;
    m_state.store(State::Running, std::memory_order_release);
    while (!m_stop_requested.load(std::memory_order_acquire)) {
        // Skeleton loop: concrete read/dispatch/schedule stages will be added in later tasks.
        // Keep dispatcher state alive so later tasks can wire in real frames.
        (void)dispatch_state;
        (void)Http2OutboundScheduler::pickSendableFrames(budget, pending_streams);
        const auto timer_event = checkTimers(std::chrono::steady_clock::now());
        if (timer_event == TimerEvent::SettingsAckTimeout ||
            timer_event == TimerEvent::PingAckTimeout ||
            timer_event == TimerEvent::GracefulShutdownTimeout) {
            requestStop();
        }
        co_await galay::kernel::sleep(std::chrono::milliseconds(1));
    }
    m_state.store(State::Stopped, std::memory_order_release);
    co_return;
}

} // namespace galay::http2
