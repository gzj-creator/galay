#include "frame_dispacher.h"

namespace galay::http2
{

namespace
{

H2DispatchResult connection_error(Http2ErrorCode code)
{
    H2DispatchResult result;
    result.ok = false;
    result.error_scope = H2DispatchErrorScope::Connection;
    result.error_code = code;
    result.actions.push_back({
        H2DispatchActionType::SendGoaway,
        0,
        code
    });
    return result;
}

H2DispatchResult stream_error(uint32_t stream_id, Http2ErrorCode code)
{
    H2DispatchResult result;
    result.ok = false;
    result.error_scope = H2DispatchErrorScope::Stream;
    result.error_code = code;
    result.actions.push_back({
        H2DispatchActionType::SendRstStream,
        stream_id,
        code
    });
    return result;
}

bool requires_non_zero_stream(const Http2Frame& frame)
{
    return frame.is_data() ||
           frame.is_headers() ||
           frame.is_priority() ||
           frame.is_rst_stream() ||
           frame.is_continuation();
}

bool requires_zero_stream(const Http2Frame& frame)
{
    return frame.is_settings() ||
           frame.is_ping() ||
           frame.is_go_away();
}

bool is_stream_frame(const Http2Frame& frame)
{
    return requires_non_zero_stream(frame) ||
           frame.is_push_promise();
}

H2DispatcherStreamState& stream_state(H2DispatcherConnectionState& state,
                                     uint32_t stream_id)
{
    return state.streams.try_emplace(stream_id).first->second;
}

bool is_remote_closed(const H2DispatcherStreamState& stream)
{
    return stream.lifecycle == H2StreamLifecycleState::HalfClosedRemote ||
           stream.lifecycle == H2StreamLifecycleState::Closed;
}

void mark_remote_end_stream(H2DispatcherStreamState& stream)
{
    switch (stream.lifecycle) {
        case H2StreamLifecycleState::Idle:
        case H2StreamLifecycleState::Open:
            stream.lifecycle = H2StreamLifecycleState::HalfClosedRemote;
            break;
        case H2StreamLifecycleState::HalfClosedLocal:
            stream.lifecycle = H2StreamLifecycleState::Closed;
            break;
        case H2StreamLifecycleState::ReservedLocal:
        case H2StreamLifecycleState::ReservedRemote:
        case H2StreamLifecycleState::HalfClosedRemote:
        case H2StreamLifecycleState::Closed:
            break;
    }
}

bool can_receive_data(const H2DispatcherStreamState& stream)
{
    return stream.lifecycle == H2StreamLifecycleState::Open ||
           stream.lifecycle == H2StreamLifecycleState::HalfClosedLocal;
}

bool is_new_stream_rejected_by_goaway(const Http2Frame& frame,
                                 const H2DispatcherConnectionState& state,
                                 uint32_t stream_id)
{
    if (!state.goaway_received || !frame.is_headers()) {
        return false;
    }
    if (stream_id <= state.goaway_last_stream_id) {
        return false;
    }
    return state.streams.find(stream_id) == state.streams.end();
}

} // namespace

H2DispatchResult Http2FrameDispatcher::dispatch(const Http2Frame& frame,
                                                H2DispatcherConnectionState& state)
{
    H2DispatchResult result;
    const uint32_t stream_id = frame.stream_id();

    if (requires_non_zero_stream(frame) && stream_id == 0) {
        return connection_error(Http2ErrorCode::ProtocolError);
    }
    if (requires_zero_stream(frame) && stream_id != 0) {
        return connection_error(Http2ErrorCode::ProtocolError);
    }

    if (state.expecting_continuation) {
        if (!frame.is_continuation() || stream_id != state.continuation_stream_id) {
            return connection_error(Http2ErrorCode::ProtocolError);
        }
    }

    if (is_new_stream_rejected_by_goaway(frame, state, stream_id)) {
        return stream_error(stream_id, Http2ErrorCode::ProtocolError);
    }

    if (frame.is_headers()) {
        auto& stream = stream_state(state, stream_id);
        if (is_remote_closed(stream)) {
            return stream_error(stream_id, Http2ErrorCode::ProtocolError);
        }
        if (stream.lifecycle == H2StreamLifecycleState::Idle) {
            stream.lifecycle = H2StreamLifecycleState::Open;
            if (stream_id > state.last_peer_stream_id) {
                state.last_peer_stream_id = stream_id;
            }
        }

        const auto* headers = frame.as_headers();
        if (headers && headers->is_end_stream()) {
            mark_remote_end_stream(stream);
        }
        if (headers && !headers->is_end_headers()) {
            state.expecting_continuation = true;
            state.continuation_stream_id = stream_id;
        } else {
            state.expecting_continuation = false;
            state.continuation_stream_id = 0;
        }
        result.actions.push_back({H2DispatchActionType::DeliverToStream, stream_id, Http2ErrorCode::NoError});
        return result;
    }

    if (frame.is_data()) {
        auto it = state.streams.find(stream_id);
        if (it == state.streams.end() || !can_receive_data(it->second)) {
            return stream_error(stream_id, Http2ErrorCode::ProtocolError);
        }
        const auto* data = frame.as_data();
        if (data && data->is_end_stream()) {
            mark_remote_end_stream(it->second);
        }
        result.actions.push_back({H2DispatchActionType::DeliverToStream, stream_id, Http2ErrorCode::NoError});
        return result;
    }

    if (frame.is_continuation()) {
        const auto* cont = frame.as_continuation();
        if (!state.expecting_continuation || stream_id != state.continuation_stream_id) {
            return connection_error(Http2ErrorCode::ProtocolError);
        }
        if (cont && cont->is_end_headers()) {
            state.expecting_continuation = false;
            state.continuation_stream_id = 0;
        }
        result.actions.push_back({H2DispatchActionType::DeliverToStream, stream_id, Http2ErrorCode::NoError});
        return result;
    }

    if (frame.is_rst_stream()) {
        auto& stream = stream_state(state, stream_id);
        stream.lifecycle = H2StreamLifecycleState::Closed;
        result.actions.push_back({H2DispatchActionType::DeliverToStream, stream_id, Http2ErrorCode::NoError});
        return result;
    }

    if (frame.is_go_away()) {
        state.goaway_received = true;
        if (const auto* goaway = frame.as_go_away()) {
            state.goaway_last_stream_id = goaway->last_stream_id();
        }
        return result;
    }

    if (frame.is_window_update()) {
        const auto* wu = frame.as_window_update();
        if (wu && wu->window_size_increment() == 0) {
            if (stream_id == 0) {
                return connection_error(Http2ErrorCode::ProtocolError);
            }
            return stream_error(stream_id, Http2ErrorCode::ProtocolError);
        }
        result.actions.push_back({H2DispatchActionType::UpdateWindow, stream_id, Http2ErrorCode::NoError});
        return result;
    }

    if (is_stream_frame(frame)) {
        result.actions.push_back({H2DispatchActionType::DeliverToStream, stream_id, Http2ErrorCode::NoError});
    }

    return result;
}

} // namespace galay::http2
