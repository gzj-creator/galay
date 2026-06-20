/**
 * @file T33-H2ConnectionCoreLifecycle.cc
 * @brief HTTP/2 connection core lifecycle contract test
 */

#include <galay/cpp/galay-http2/kernel/h2_core.h>
#include <galay/cpp/galay-http2/builder/http2_frame_builder.h>
#include <cassert>
#include <iostream>

using namespace galay::http2;

int main() {
    Http2ConnectionCore core;
    assert(core.state() == Http2ConnectionCore::State::Idle);

    core.markSettingsSent();
    assert(core.isSettingsAckPending());

    core.markSettingsAcked();
    assert(!core.isSettingsAckPending());

    core.requestStop();
    assert(core.stopRequested());

    Http2ConnectionCore event_core;
    Http2DataFrame bad_data;
    bad_data.header().stream_id = 0;
    auto bad_result = event_core.receiveFrame(bad_data);
    assert(!bad_result.ok);
    assert(bad_result.error_scope == H2DispatchErrorScope::Connection);
    assert(event_core.hasOutboundWork());

    auto control = event_core.flushOutbound(H2OutboundBudget{
        .conn_window = 0,
        .max_frame_size = 4
    });
    assert(control.frames.size() == 1);
    assert(control.frames[0]->isGoAway());
    assert(!event_core.hasOutboundWork());

    Http2ConnectionCore data_core;
    data_core.enqueueData(1, "abcd", false);
    auto blocked = data_core.flushOutbound(H2OutboundBudget{
        .conn_window = 0,
        .max_frame_size = 4
    });
    assert(blocked.frames.empty());

    Http2WindowUpdateFrame window_update;
    window_update.header().stream_id = 0;
    window_update.setWindowSizeIncrement(4);
    auto window_result = data_core.receiveFrame(window_update);
    assert(window_result.ok);
    assert(data_core.outboundReady());

    auto unblocked = data_core.flushOutbound(H2OutboundBudget{
        .conn_window = 4,
        .max_frame_size = 4
    });
    assert(unblocked.frames.size() == 1);
    assert(unblocked.frames[0]->isData());
    assert(unblocked.total_data_bytes == 4);

    Http2ConnectionCore bytes_core;
    bytes_core.enqueueData(3, "abcdefgh", true);
    auto bytes = bytes_core.flushOutboundBytes(H2OutboundBudget{
        .conn_window = 8,
        .max_frame_size = 4
    });
    assert(bytes.frames.size() == 2);
    assert(bytes.frames[0] == Http2FrameBuilder::dataBytes(3, "abcd", false));
    assert(bytes.frames[1] == Http2FrameBuilder::dataBytes(3, "efgh", true));
    assert(bytes.total_data_bytes == 8);
    assert(!bytes_core.hasOutboundWork());

    Http2ConnectionCore control_bytes_core;
    Http2DataFrame invalid_data;
    invalid_data.header().stream_id = 0;
    auto invalid_result = control_bytes_core.receiveFrame(invalid_data);
    assert(!invalid_result.ok);
    auto control_bytes = control_bytes_core.flushOutboundBytes(H2OutboundBudget{
        .conn_window = 0,
        .max_frame_size = 4
    });
    assert(control_bytes.frames.size() == 1);
    assert(control_bytes.frames[0].size() >= kHttp2FrameHeaderLength);
    assert(!control_bytes_core.hasOutboundWork());

    std::cout << "T33-H2ConnectionCoreLifecycle PASS\n";
    return 0;
}
