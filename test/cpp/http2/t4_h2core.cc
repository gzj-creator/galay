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

    core.mark_settings_sent();
    assert(core.is_settings_ack_pending());

    core.mark_settings_acked();
    assert(!core.is_settings_ack_pending());

    core.request_stop();
    assert(core.stop_requested());

    Http2ConnectionCore event_core;
    Http2DataFrame bad_data;
    bad_data.header().stream_id = 0;
    auto bad_result = event_core.receive_frame(bad_data);
    assert(!bad_result.ok);
    assert(bad_result.error_scope == H2DispatchErrorScope::Connection);
    assert(event_core.has_outbound_work());

    auto control = event_core.flush_outbound(H2OutboundBudget{
        .conn_window = 0,
        .max_frame_size = 4
    });
    assert(control.frames.size() == 1);
    assert(control.frames[0]->is_go_away());
    assert(!event_core.has_outbound_work());

    Http2ConnectionCore data_core;
    data_core.enqueue_data(1, "abcd", false);
    auto blocked = data_core.flush_outbound(H2OutboundBudget{
        .conn_window = 0,
        .max_frame_size = 4
    });
    assert(blocked.frames.empty());

    Http2WindowUpdateFrame window_update;
    window_update.header().stream_id = 0;
    window_update.set_window_size_increment(4);
    auto window_result = data_core.receive_frame(window_update);
    assert(window_result.ok);
    assert(data_core.outbound_ready());

    auto unblocked = data_core.flush_outbound(H2OutboundBudget{
        .conn_window = 4,
        .max_frame_size = 4
    });
    assert(unblocked.frames.size() == 1);
    assert(unblocked.frames[0]->is_data());
    assert(unblocked.total_data_bytes == 4);

    Http2ConnectionCore bytes_core;
    bytes_core.enqueue_data(3, "abcdefgh", true);
    auto bytes = bytes_core.flush_outbound_bytes(H2OutboundBudget{
        .conn_window = 8,
        .max_frame_size = 4
    });
    assert(bytes.frames.size() == 2);
    assert(bytes.frames[0] == Http2FrameBuilder::data_bytes(3, "abcd", false));
    assert(bytes.frames[1] == Http2FrameBuilder::data_bytes(3, "efgh", true));
    assert(bytes.total_data_bytes == 8);
    assert(!bytes_core.has_outbound_work());

    Http2ConnectionCore control_bytes_core;
    Http2DataFrame invalid_data;
    invalid_data.header().stream_id = 0;
    auto invalid_result = control_bytes_core.receive_frame(invalid_data);
    assert(!invalid_result.ok);
    auto control_bytes = control_bytes_core.flush_outbound_bytes(H2OutboundBudget{
        .conn_window = 0,
        .max_frame_size = 4
    });
    assert(control_bytes.frames.size() == 1);
    assert(control_bytes.frames[0].size() >= kHttp2FrameHeaderLength);
    assert(!control_bytes_core.has_outbound_work());

    std::cout << "T33-H2ConnectionCoreLifecycle PASS\n";
    return 0;
}
