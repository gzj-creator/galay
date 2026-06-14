#include "frame_disp.h"

namespace galay::http2
{

H2DispatchResult Http2FrameDispatcher::dispatch(const Http2Frame& frame,
                                                H2DispatcherConnectionState& state)
{
    H2DispatchResult result;
    const uint32_t stream_id = frame.streamId();

    if (state.expecting_continuation) {
        if (!frame.isContinuation() || stream_id != state.continuation_stream_id) {
            result.ok = false;
            result.actions.push_back({
                H2DispatchActionType::SendGoaway,
                0,
                Http2ErrorCode::ProtocolError
            });
            return result;
        }
    }

    if (frame.isHeaders()) {
        const auto* headers = frame.asHeaders();
        if (headers && !headers->isEndHeaders()) {
            state.expecting_continuation = true;
            state.continuation_stream_id = stream_id;
        } else {
            state.expecting_continuation = false;
            state.continuation_stream_id = 0;
        }
        return result;
    }

    if (frame.isContinuation()) {
        const auto* cont = frame.asContinuation();
        if (!state.expecting_continuation || stream_id != state.continuation_stream_id) {
            result.ok = false;
            result.actions.push_back({
                H2DispatchActionType::SendGoaway,
                0,
                Http2ErrorCode::ProtocolError
            });
            return result;
        }
        if (cont && cont->isEndHeaders()) {
            state.expecting_continuation = false;
            state.continuation_stream_id = 0;
        }
        return result;
    }

    if (frame.isGoAway()) {
        state.goaway_received = true;
        return result;
    }

    if (frame.isWindowUpdate() && stream_id == 0) {
        const auto* wu = frame.asWindowUpdate();
        if (wu && wu->windowSizeIncrement() == 0) {
            result.ok = false;
            result.actions.push_back({
                H2DispatchActionType::SendGoaway,
                0,
                Http2ErrorCode::ProtocolError
            });
        }
        return result;
    }

    return result;
}

} // namespace galay::http2
