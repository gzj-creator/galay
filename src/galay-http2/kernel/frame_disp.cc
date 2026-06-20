#include "frame_disp.h"

namespace galay::http2
{

namespace
{

H2DispatchResult connectionError(Http2ErrorCode code)
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

H2DispatchResult streamError(uint32_t stream_id, Http2ErrorCode code)
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

bool requiresNonZeroStream(const Http2Frame& frame)
{
    return frame.isData() ||
           frame.isHeaders() ||
           frame.isPriority() ||
           frame.isRstStream() ||
           frame.isContinuation();
}

bool requiresZeroStream(const Http2Frame& frame)
{
    return frame.isSettings() ||
           frame.isPing() ||
           frame.isGoAway();
}

bool isStreamFrame(const Http2Frame& frame)
{
    return requiresNonZeroStream(frame) ||
           frame.isPushPromise();
}

H2DispatcherStreamState& streamState(H2DispatcherConnectionState& state,
                                     uint32_t stream_id)
{
    return state.streams.try_emplace(stream_id).first->second;
}

bool isRemoteClosed(const H2DispatcherStreamState& stream)
{
    return stream.lifecycle == H2StreamLifecycleState::HalfClosedRemote ||
           stream.lifecycle == H2StreamLifecycleState::Closed;
}

void markRemoteEndStream(H2DispatcherStreamState& stream)
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

bool canReceiveData(const H2DispatcherStreamState& stream)
{
    return stream.lifecycle == H2StreamLifecycleState::Open ||
           stream.lifecycle == H2StreamLifecycleState::HalfClosedLocal;
}

bool isNewStreamRejectedByGoaway(const Http2Frame& frame,
                                 const H2DispatcherConnectionState& state,
                                 uint32_t stream_id)
{
    if (!state.goaway_received || !frame.isHeaders()) {
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
    const uint32_t stream_id = frame.streamId();

    if (requiresNonZeroStream(frame) && stream_id == 0) {
        return connectionError(Http2ErrorCode::ProtocolError);
    }
    if (requiresZeroStream(frame) && stream_id != 0) {
        return connectionError(Http2ErrorCode::ProtocolError);
    }

    if (state.expecting_continuation) {
        if (!frame.isContinuation() || stream_id != state.continuation_stream_id) {
            return connectionError(Http2ErrorCode::ProtocolError);
        }
    }

    if (isNewStreamRejectedByGoaway(frame, state, stream_id)) {
        return streamError(stream_id, Http2ErrorCode::ProtocolError);
    }

    if (frame.isHeaders()) {
        auto& stream = streamState(state, stream_id);
        if (isRemoteClosed(stream)) {
            return streamError(stream_id, Http2ErrorCode::ProtocolError);
        }
        if (stream.lifecycle == H2StreamLifecycleState::Idle) {
            stream.lifecycle = H2StreamLifecycleState::Open;
            if (stream_id > state.last_peer_stream_id) {
                state.last_peer_stream_id = stream_id;
            }
        }

        const auto* headers = frame.asHeaders();
        if (headers && headers->isEndStream()) {
            markRemoteEndStream(stream);
        }
        if (headers && !headers->isEndHeaders()) {
            state.expecting_continuation = true;
            state.continuation_stream_id = stream_id;
        } else {
            state.expecting_continuation = false;
            state.continuation_stream_id = 0;
        }
        result.actions.push_back({H2DispatchActionType::DeliverToStream, stream_id, Http2ErrorCode::NoError});
        return result;
    }

    if (frame.isData()) {
        auto it = state.streams.find(stream_id);
        if (it == state.streams.end() || !canReceiveData(it->second)) {
            return streamError(stream_id, Http2ErrorCode::ProtocolError);
        }
        const auto* data = frame.asData();
        if (data && data->isEndStream()) {
            markRemoteEndStream(it->second);
        }
        result.actions.push_back({H2DispatchActionType::DeliverToStream, stream_id, Http2ErrorCode::NoError});
        return result;
    }

    if (frame.isContinuation()) {
        const auto* cont = frame.asContinuation();
        if (!state.expecting_continuation || stream_id != state.continuation_stream_id) {
            return connectionError(Http2ErrorCode::ProtocolError);
        }
        if (cont && cont->isEndHeaders()) {
            state.expecting_continuation = false;
            state.continuation_stream_id = 0;
        }
        result.actions.push_back({H2DispatchActionType::DeliverToStream, stream_id, Http2ErrorCode::NoError});
        return result;
    }

    if (frame.isRstStream()) {
        auto& stream = streamState(state, stream_id);
        stream.lifecycle = H2StreamLifecycleState::Closed;
        result.actions.push_back({H2DispatchActionType::DeliverToStream, stream_id, Http2ErrorCode::NoError});
        return result;
    }

    if (frame.isGoAway()) {
        state.goaway_received = true;
        if (const auto* goaway = frame.asGoAway()) {
            state.goaway_last_stream_id = goaway->lastStreamId();
        }
        return result;
    }

    if (frame.isWindowUpdate()) {
        const auto* wu = frame.asWindowUpdate();
        if (wu && wu->windowSizeIncrement() == 0) {
            if (stream_id == 0) {
                return connectionError(Http2ErrorCode::ProtocolError);
            }
            return streamError(stream_id, Http2ErrorCode::ProtocolError);
        }
        result.actions.push_back({H2DispatchActionType::UpdateWindow, stream_id, Http2ErrorCode::NoError});
        return result;
    }

    if (isStreamFrame(frame)) {
        result.actions.push_back({H2DispatchActionType::DeliverToStream, stream_id, Http2ErrorCode::NoError});
    }

    return result;
}

} // namespace galay::http2
