/**
 * @file T34-H2DispatcherStateMachine.cc
 * @brief Dispatcher state machine contract test
 */

#include "galay-http2/kernel/frame_disp.h"
#include <cassert>
#include <iostream>

using namespace galay::http2;

int main() {
    H2DispatcherConnectionState state;

    // HEADERS without END_HEADERS must arm continuation expectation.
    Http2HeadersFrame headers;
    headers.header().stream_id = 1;
    headers.setEndHeaders(false);
    auto r1 = Http2FrameDispatcher::dispatch(headers, state);
    assert(r1.ok);
    assert(state.expecting_continuation);
    assert(state.continuation_stream_id == 1);

    // CONTINUATION on different stream must fail with GOAWAY action.
    Http2ContinuationFrame bad_cont;
    bad_cont.header().stream_id = 3;
    bad_cont.setEndHeaders(true);
    auto r2 = Http2FrameDispatcher::dispatch(bad_cont, state);
    assert(!r2.ok);
    assert(!r2.actions.empty());
    assert(r2.actions.front().type == H2DispatchActionType::SendGoaway);
    assert(r2.actions.front().error_code == Http2ErrorCode::ProtocolError);

    // Happy path continuation closes expectation.
    state.expecting_continuation = true;
    state.continuation_stream_id = 1;
    Http2ContinuationFrame good_cont;
    good_cont.header().stream_id = 1;
    good_cont.setEndHeaders(true);
    auto r3 = Http2FrameDispatcher::dispatch(good_cont, state);
    assert(r3.ok);
    assert(!state.expecting_continuation);

    std::cout << "T34-H2DispatcherStateMachine PASS\n";
    return 0;
}
