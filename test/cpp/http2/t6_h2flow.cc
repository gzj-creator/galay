/**
 * @file T35-H2FlowControl.cc
 * @brief Outbound scheduler flow-control contract test
 */

#include <galay/cpp/galay-http2/kernel/flow_control.h>
#include <galay/cpp/galay-http2/kernel/out_sched.h>
#include <cassert>
#include <iostream>
#include <memory>

using namespace galay::http2;

namespace
{

size_t dataBytesForStream(const H2OutboundSelection& selection, uint32_t stream_id)
{
    size_t bytes = 0;
    for (const auto& frame : selection.frames) {
        auto* data = frame->asData();
        if (data && data->streamId() == stream_id) {
            bytes += data->data().size();
        }
    }
    return bytes;
}

} // namespace

int main() {
    H2FlowController flow;
    assert(flow.ensureStream(1).has_value());
    assert(flow.ensureStream(3).has_value());

    assert(flow.availableToSend(1, 100000, 100000) == kDefaultInitialWindowSize);
    assert(flow.consumeSendWindow(1, 60000).has_value());
    assert(flow.availableToSend(1, 100000, 100000) == 5535);

    // Connection send window is shared across streams.
    assert(flow.availableToSend(3, 100000, 100000) == 5535);
    assert(flow.applyConnectionWindowUpdate(60000).has_value());
    assert(flow.availableToSend(3, 100000, 100000) == kDefaultInitialWindowSize);

    // Stream send window is independent from the connection window.
    assert(flow.consumeSendWindow(1, 5000).has_value());
    assert(flow.availableToSend(1, 100000, 100000) == 535);
    assert(flow.applyStreamWindowUpdate(1, 1000).has_value());
    assert(flow.availableToSend(1, 100000, 100000) == 1535);

    // SETTINGS_INITIAL_WINDOW_SIZE applies the delta to all known stream windows.
    assert(flow.applyConnectionWindowUpdate(70000).has_value());
    assert(flow.applyInitialStreamWindowSize(70000).has_value());
    assert(flow.availableToSend(1, 100000, 100000) == 6000);
    assert(flow.availableToSend(3, 100000, 100000) == 70000);

    auto overflow = flow.applyConnectionWindowUpdate(2147483647u - kDefaultInitialWindowSize + 1u);
    assert(!overflow.has_value());
    assert(overflow.error() == H2FlowControlError::WindowOverflow);

    auto unknown_stream = flow.applyStreamWindowUpdate(99, 1);
    assert(!unknown_stream.has_value());
    assert(unknown_stream.error() == H2FlowControlError::UnknownStream);

    H2OutboundBudget budget{
        .conn_window = 6,
        .max_frame_size = 4
    };

    std::vector<H2StreamSendState> streams;
    streams.push_back(H2StreamSendState{
        .pending = {
            .chunks = {"1234567890"},
            .front_offset = 0,
            .end_stream = true
        },
        .stream_id = 1,
        .stream_window = 6,
        .weight = 16
    });

    auto selected = Http2OutboundScheduler::pickSendableFrames(budget, streams);
    assert(selected.frames.size() == 2);
    assert(selected.total_data_bytes == 6);

    auto* d1 = selected.frames[0]->asData();
    auto* d2 = selected.frames[1]->asData();
    assert(d1 && d2);
    assert(d1->data().size() == 4);
    assert(d2->data().size() == 2);
    assert(!d1->isEndStream());
    assert(!d2->isEndStream());
    assert(streams[0].pending.chunks.size() == 1);
    assert(streams[0].pending.chunks.front() == "1234567890");
    assert(streams[0].pending.front_offset == 6);

    // Simulate WINDOW_UPDATE on conn/stream then continue sending remains.
    budget.conn_window = 8;
    streams[0].stream_window += 8;
    auto selected2 = Http2OutboundScheduler::pickSendableFrames(budget, streams);
    assert(selected2.total_data_bytes == 4);
    assert(selected2.frames.size() == 1);
    auto* d3 = selected2.frames[0]->asData();
    assert(d3);
    assert(d3->data() == "7890");
    assert(d3->isEndStream());
    assert(streams[0].pending.chunks.empty());
    assert(streams[0].pending.front_offset == 0);

    std::vector<H2StreamSendState> empty_streams;
    empty_streams.push_back(H2StreamSendState{
        .pending = {
            .chunks = {},
            .front_offset = 0,
            .end_stream = true
        },
        .stream_id = 11,
        .stream_window = 6,
        .weight = 16
    });

    auto empty_end = Http2OutboundScheduler::pickSendableFrames(budget, empty_streams);
    assert(empty_end.frames.size() == 1);
    assert(empty_end.total_data_bytes == 0);
    auto* empty_data = empty_end.frames[0]->asData();
    assert(empty_data);
    assert(empty_data->data().empty());
    assert(empty_data->isEndStream());

    auto empty_again = Http2OutboundScheduler::pickSendableFrames(budget, empty_streams);
    assert(empty_again.frames.empty());

    H2OutboundBudget fair_budget{
        .conn_window = 10,
        .max_frame_size = 2
    };
    std::vector<H2StreamSendState> fair_streams;
    fair_streams.push_back(H2StreamSendState{
        .pending = {
            .chunks = {"llllllllll"},
            .front_offset = 0,
            .end_stream = false
        },
        .stream_id = 1,
        .stream_window = 20,
        .weight = 1
    });
    fair_streams.push_back(H2StreamSendState{
        .pending = {
            .chunks = {"HHHHHHHHHH"},
            .front_offset = 0,
            .end_stream = false
        },
        .stream_id = 3,
        .stream_window = 20,
        .weight = 4
    });

    auto fair = Http2OutboundScheduler::pickSendableFrames(fair_budget, fair_streams, H2SchedulerConfig{
        .base_quantum = 2
    });
    const size_t low_weight_bytes = dataBytesForStream(fair, 1);
    const size_t high_weight_bytes = dataBytesForStream(fair, 3);
    assert(low_weight_bytes > 0);
    assert(high_weight_bytes > low_weight_bytes);
    assert(fair_streams[0].stream_id == 1);
    assert(fair_streams[1].stream_id == 3);

    H2OutboundQueues priority_queues;
    auto settings_ack = std::make_unique<Http2SettingsFrame>();
    settings_ack->setAck(true);
    priority_queues.control_frames.push_back(std::move(settings_ack));
    auto ping_ack = std::make_unique<Http2PingFrame>();
    ping_ack->setAck(true);
    priority_queues.control_frames.push_back(std::move(ping_ack));
    priority_queues.control_frames.push_back(Http2FrameBuilder::rstStream(1, Http2ErrorCode::Cancel));
    priority_queues.control_frames.push_back(std::make_unique<Http2GoAwayFrame>());
    priority_queues.header_frames.push_back(Http2FrameBuilder::headers(3, "headers", false, true));
    priority_queues.data_streams.push_back(H2StreamSendState{
        .pending = {
            .chunks = {"data"},
            .front_offset = 0,
            .end_stream = false
        },
        .stream_id = 3,
        .stream_window = 20,
        .weight = 16
    });

    auto priority_selection = Http2OutboundScheduler::pickSendableFrames(H2OutboundBudget{
        .conn_window = 4,
        .max_frame_size = 4
    }, priority_queues);
    assert(priority_selection.frames.size() == 6);
    assert(priority_selection.frames[0]->isSettings());
    assert(priority_selection.frames[1]->isPing());
    assert(priority_selection.frames[2]->isRstStream());
    assert(priority_selection.frames[3]->isGoAway());
    assert(priority_selection.frames[4]->isHeaders());
    assert(priority_selection.frames[5]->isData());
    assert(priority_selection.total_data_bytes == 4);

    H2OutboundQueues blocked_queues;
    blocked_queues.header_frames.push_back(Http2FrameBuilder::headers(5, "headers", false, true));
    blocked_queues.data_streams.push_back(H2StreamSendState{
        .pending = {
            .chunks = {"blocked"},
            .front_offset = 0,
            .end_stream = false
        },
        .stream_id = 5,
        .stream_window = 20,
        .weight = 16
    });
    auto blocked_selection = Http2OutboundScheduler::pickSendableFrames(H2OutboundBudget{
        .conn_window = 0,
        .max_frame_size = 4
    }, blocked_queues);
    assert(blocked_selection.frames.size() == 1);
    assert(blocked_selection.frames[0]->isHeaders());
    assert(blocked_selection.total_data_bytes == 0);
    assert(!blocked_queues.data_streams[0].pending.chunks.empty());

    std::vector<H2StreamSendState> byte_streams;
    byte_streams.push_back(H2StreamSendState{
        .pending = {
            .chunks = {"abcdefgh"},
            .front_offset = 0,
            .end_stream = true
        },
        .stream_id = 13,
        .stream_window = 8,
        .weight = 16
    });
    auto byte_selection = Http2OutboundScheduler::pickSendableBytes(H2OutboundBudget{
        .conn_window = 8,
        .max_frame_size = 4
    }, byte_streams);
    assert(byte_selection.frames.size() == 2);
    assert(byte_selection.total_data_bytes == 8);
    assert(byte_selection.frames[0] == Http2FrameBuilder::dataBytes(13, "abcd", false));
    assert(byte_selection.frames[1] == Http2FrameBuilder::dataBytes(13, "efgh", true));
    assert(byte_streams[0].pending.chunks.empty());

    std::cout << "T35-H2FlowControl PASS\n";
    return 0;
}
