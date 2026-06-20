/**
 * @file T34-H2DispatcherStateMachine.cc
 * @brief Dispatcher state machine contract test
 */

#include <galay/cpp/galay-http2/kernel/frame_disp.h>
#include <cassert>
#include <iostream>

using namespace galay::http2;

namespace
{

void expectConnectionError(const H2DispatchResult& result)
{
    assert(!result.ok);
    assert(result.error_scope == H2DispatchErrorScope::Connection);
    assert(result.error_code == Http2ErrorCode::ProtocolError);
    assert(!result.actions.empty());
    assert(result.actions.front().type == H2DispatchActionType::SendGoaway);
    assert(result.actions.front().error_code == Http2ErrorCode::ProtocolError);
}

void expectStreamError(const H2DispatchResult& result, uint32_t stream_id)
{
    assert(!result.ok);
    assert(result.error_scope == H2DispatchErrorScope::Stream);
    assert(result.error_code == Http2ErrorCode::ProtocolError);
    assert(!result.actions.empty());
    assert(result.actions.front().type == H2DispatchActionType::SendRstStream);
    assert(result.actions.front().stream_id == stream_id);
    assert(result.actions.front().error_code == Http2ErrorCode::ProtocolError);
}

void expectDelivered(const H2DispatchResult& result, uint32_t stream_id)
{
    assert(result.ok);
    assert(!result.actions.empty());
    assert(result.actions.front().type == H2DispatchActionType::DeliverToStream);
    assert(result.actions.front().stream_id == stream_id);
}

void expectLifecycle(const H2DispatcherConnectionState& state,
                     uint32_t stream_id,
                     H2StreamLifecycleState lifecycle)
{
    auto it = state.streams.find(stream_id);
    assert(it != state.streams.end());
    assert(it->second.lifecycle == lifecycle);
}

} // namespace

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
    assert(r2.error_scope == H2DispatchErrorScope::Connection);
    assert(r2.error_code == Http2ErrorCode::ProtocolError);
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

    // Frames that operate on a stream must not use stream 0.
    H2DispatcherConnectionState stream_id_state;

    Http2DataFrame data_on_conn;
    data_on_conn.header().stream_id = 0;
    expectConnectionError(Http2FrameDispatcher::dispatch(data_on_conn, stream_id_state));

    Http2HeadersFrame headers_on_conn;
    headers_on_conn.header().stream_id = 0;
    headers_on_conn.setEndHeaders(true);
    expectConnectionError(Http2FrameDispatcher::dispatch(headers_on_conn, stream_id_state));

    Http2PriorityFrame priority_on_conn;
    priority_on_conn.header().stream_id = 0;
    expectConnectionError(Http2FrameDispatcher::dispatch(priority_on_conn, stream_id_state));

    Http2RstStreamFrame rst_on_conn;
    rst_on_conn.header().stream_id = 0;
    expectConnectionError(Http2FrameDispatcher::dispatch(rst_on_conn, stream_id_state));

    Http2ContinuationFrame continuation_on_conn;
    continuation_on_conn.header().stream_id = 0;
    continuation_on_conn.setEndHeaders(true);
    expectConnectionError(Http2FrameDispatcher::dispatch(continuation_on_conn, stream_id_state));

    // Connection-level frames must use stream 0.
    Http2SettingsFrame settings_on_stream;
    settings_on_stream.header().stream_id = 1;
    expectConnectionError(Http2FrameDispatcher::dispatch(settings_on_stream, stream_id_state));

    Http2PingFrame ping_on_stream;
    ping_on_stream.header().stream_id = 1;
    expectConnectionError(Http2FrameDispatcher::dispatch(ping_on_stream, stream_id_state));

    Http2GoAwayFrame goaway_on_stream;
    goaway_on_stream.header().stream_id = 1;
    expectConnectionError(Http2FrameDispatcher::dispatch(goaway_on_stream, stream_id_state));

    // WINDOW_UPDATE increment 0 is connection error on stream 0 and stream error on stream frames.
    Http2WindowUpdateFrame bad_conn_window;
    bad_conn_window.header().stream_id = 0;
    bad_conn_window.setWindowSizeIncrement(0);
    expectConnectionError(Http2FrameDispatcher::dispatch(bad_conn_window, stream_id_state));

    Http2WindowUpdateFrame bad_stream_window;
    bad_stream_window.header().stream_id = 3;
    bad_stream_window.setWindowSizeIncrement(0);
    expectStreamError(Http2FrameDispatcher::dispatch(bad_stream_window, stream_id_state), 3);

    // HEADERS on idle stream opens the stream.
    H2DispatcherConnectionState lifecycle_state;
    Http2HeadersFrame open_headers;
    open_headers.header().stream_id = 5;
    open_headers.setEndHeaders(true);
    auto open_result = Http2FrameDispatcher::dispatch(open_headers, lifecycle_state);
    expectDelivered(open_result, 5);
    expectLifecycle(lifecycle_state, 5, H2StreamLifecycleState::Open);

    // HEADERS with END_STREAM opens and immediately half-closes the remote side.
    Http2HeadersFrame end_headers;
    end_headers.header().stream_id = 7;
    end_headers.setEndHeaders(true);
    end_headers.setEndStream(true);
    auto half_closed_from_headers = Http2FrameDispatcher::dispatch(end_headers, lifecycle_state);
    expectDelivered(half_closed_from_headers, 7);
    expectLifecycle(lifecycle_state, 7, H2StreamLifecycleState::HalfClosedRemote);

    // DATA with END_STREAM moves an open stream to half-closed remote.
    Http2HeadersFrame data_stream_headers;
    data_stream_headers.header().stream_id = 9;
    data_stream_headers.setEndHeaders(true);
    expectDelivered(Http2FrameDispatcher::dispatch(data_stream_headers, lifecycle_state), 9);
    expectLifecycle(lifecycle_state, 9, H2StreamLifecycleState::Open);

    Http2DataFrame end_data;
    end_data.header().stream_id = 9;
    end_data.setEndStream(true);
    expectDelivered(Http2FrameDispatcher::dispatch(end_data, lifecycle_state), 9);
    expectLifecycle(lifecycle_state, 9, H2StreamLifecycleState::HalfClosedRemote);

    // RST_STREAM closes a stream; closed streams reject further DATA/HEADERS.
    Http2RstStreamFrame rst;
    rst.header().stream_id = 9;
    expectDelivered(Http2FrameDispatcher::dispatch(rst, lifecycle_state), 9);
    expectLifecycle(lifecycle_state, 9, H2StreamLifecycleState::Closed);

    Http2DataFrame data_after_closed;
    data_after_closed.header().stream_id = 9;
    expectStreamError(Http2FrameDispatcher::dispatch(data_after_closed, lifecycle_state), 9);

    Http2HeadersFrame headers_after_closed;
    headers_after_closed.header().stream_id = 9;
    headers_after_closed.setEndHeaders(true);
    expectStreamError(Http2FrameDispatcher::dispatch(headers_after_closed, lifecycle_state), 9);

    // GOAWAY records the last accepted stream and rejects newer streams only.
    H2DispatcherConnectionState goaway_state;
    Http2HeadersFrame existing_headers;
    existing_headers.header().stream_id = 3;
    existing_headers.setEndHeaders(true);
    expectDelivered(Http2FrameDispatcher::dispatch(existing_headers, goaway_state), 3);

    Http2GoAwayFrame goaway;
    goaway.setLastStreamId(3);
    auto goaway_result = Http2FrameDispatcher::dispatch(goaway, goaway_state);
    assert(goaway_result.ok);
    assert(goaway_state.goaway_received);
    assert(goaway_state.goaway_last_stream_id == 3);

    Http2HeadersFrame rejected_headers;
    rejected_headers.header().stream_id = 5;
    rejected_headers.setEndHeaders(true);
    expectStreamError(Http2FrameDispatcher::dispatch(rejected_headers, goaway_state), 5);

    Http2DataFrame existing_data_after_goaway;
    existing_data_after_goaway.header().stream_id = 3;
    expectDelivered(Http2FrameDispatcher::dispatch(existing_data_after_goaway, goaway_state), 3);

    Http2RstStreamFrame existing_rst_after_goaway;
    existing_rst_after_goaway.header().stream_id = 3;
    expectDelivered(Http2FrameDispatcher::dispatch(existing_rst_after_goaway, goaway_state), 3);
    expectLifecycle(goaway_state, 3, H2StreamLifecycleState::Closed);

    std::cout << "T34-H2DispatcherStateMachine PASS\n";
    return 0;
}
