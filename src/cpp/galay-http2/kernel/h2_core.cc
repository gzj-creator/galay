#include "h2_core.h"
#include "../../galay-kernel/common/sleep.hpp"

namespace galay::http2
{

namespace
{

bool has_pending_data(const H2PendingData& pending)
{
    return pending.end_stream || !pending.chunks.empty();
}

bool has_queued_data(const std::vector<H2StreamSendState>& streams)
{
    for (const auto& stream : streams) {
        if (has_pending_data(stream.pending)) {
            return true;
        }
    }
    return false;
}

} // namespace

Http2ConnectionCore::TimerEvent Http2ConnectionCore::check_timers(std::chrono::steady_clock::time_point now) noexcept
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

bool Http2ConnectionCore::has_outbound_work() const noexcept
{
    return !m_outbound_queues.control_frames.empty() ||
           !m_outbound_queues.header_frames.empty() ||
           has_queued_data(m_outbound_queues.data_streams);
}

bool Http2ConnectionCore::accepts_new_streams() const noexcept
{
    const auto current = state();
    return current == State::Idle || current == State::Running;
}

void Http2ConnectionCore::apply_timer_event(TimerEvent event) noexcept
{
    if (event == TimerEvent::SettingsAckTimeout ||
        event == TimerEvent::PingAckTimeout ||
        event == TimerEvent::GracefulShutdownTimeout) {
        force_close();
    }
}

void Http2ConnectionCore::enqueue_dispatch_action(const H2DispatchAction& action)
{
    switch (action.type) {
        case H2DispatchActionType::SendGoaway: {
            auto frame = std::make_unique<Http2GoAwayFrame>();
            frame->set_error_code(action.error_code);
            m_outbound_queues.control_frames.push_back(std::move(frame));
            m_outbound_ready = true;
            break;
        }
        case H2DispatchActionType::SendRstStream:
            m_outbound_queues.control_frames.push_back(
                Http2FrameBuilder::rst_stream(action.stream_id, action.error_code));
            m_outbound_ready = true;
            break;
        case H2DispatchActionType::AckSettings: {
            auto frame = std::make_unique<Http2SettingsFrame>();
            frame->set_ack(true);
            m_outbound_queues.control_frames.push_back(std::move(frame));
            m_outbound_ready = true;
            break;
        }
        case H2DispatchActionType::AckPing: {
            auto frame = std::make_unique<Http2PingFrame>();
            frame->set_ack(true);
            m_outbound_queues.control_frames.push_back(std::move(frame));
            m_outbound_ready = true;
            break;
        }
        case H2DispatchActionType::UpdateWindow:
            m_outbound_ready = true;
            break;
        case H2DispatchActionType::DeliverToStream:
        case H2DispatchActionType::Ignore:
            break;
    }
}

H2DispatchResult Http2ConnectionCore::receive_frame(const Http2Frame& frame)
{
    auto result = Http2FrameDispatcher::dispatch(frame, m_dispatch_state);
    for (const auto& action : result.actions) {
        enqueue_dispatch_action(action);
    }
    return result;
}

void Http2ConnectionCore::enqueue_data(uint32_t stream_id,
                                      std::string data,
                                      bool end_stream,
                                      uint8_t weight)
{
    H2StreamSendState stream;
    stream.stream_id = stream_id;
    stream.stream_window = static_cast<int32_t>(kDefaultInitialWindowSize);
    stream.pending.end_stream = end_stream;
    stream.weight = weight;
    if (!data.empty()) {
        stream.pending.chunks.push_back(std::move(data));
    }
    m_outbound_queues.data_streams.push_back(std::move(stream));
    m_outbound_ready = true;
}

H2OutboundSelection Http2ConnectionCore::flush_outbound(H2OutboundBudget budget,
                                                       H2SchedulerConfig config)
{
    auto selection = Http2OutboundScheduler::pick_sendable_frames(budget, m_outbound_queues, config);
    m_outbound_ready = has_outbound_work();
    return selection;
}

H2OutboundBytesSelection Http2ConnectionCore::flush_outbound_bytes(H2OutboundBudget budget,
                                                                 H2SchedulerConfig config)
{
    auto selection = Http2OutboundScheduler::pick_sendable_bytes(budget, m_outbound_queues, config);
    m_outbound_ready = has_outbound_work();
    return selection;
}

galay::kernel::Task<void> Http2ConnectionCore::run()
{
    if (state() == State::Idle) {
        m_state.store(State::Running, std::memory_order_release);
    }
    while (!m_stop_requested.load(std::memory_order_acquire)) {
        // Skeleton loop: actual I/O owners call receive_frame()/flush_outbound() on events.
        const auto timer_event = check_timers(std::chrono::steady_clock::now());
        apply_timer_event(timer_event);
        co_await galay::kernel::sleep(std::chrono::milliseconds(1));
    }
    m_state.store(State::Stopped, std::memory_order_release);
    co_return;
}

} // namespace galay::http2
