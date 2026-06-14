/**
 * @file T35-H2FlowControl.cc
 * @brief Outbound scheduler flow-control contract test
 */

#include "http2/kernel/out_sched.h"
#include <cassert>
#include <iostream>

using namespace galay::http2;

int main() {
    H2OutboundBudget budget{
        .conn_window = 6,
        .max_frame_size = 4
    };

    std::vector<H2StreamSendState> streams;
    streams.push_back(H2StreamSendState{
        .stream_id = 1,
        .stream_window = 6,
        .pending_data = "1234567890",
        .end_stream = true,
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

    // Simulate WINDOW_UPDATE on conn/stream then continue sending remains.
    budget.conn_window = 8;
    streams[0].stream_window += 8;
    auto selected2 = Http2OutboundScheduler::pickSendableFrames(budget, streams);
    assert(selected2.total_data_bytes == 4);
    assert(selected2.frames.size() == 1);
    auto* d3 = selected2.frames[0]->asData();
    assert(d3);
    assert(d3->isEndStream());

    std::cout << "T35-H2FlowControl PASS\n";
    return 0;
}
