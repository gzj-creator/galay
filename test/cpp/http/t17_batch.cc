/**
 * @file T50-H2ActiveBatchFlush.cc
 * @brief HTTP/2 active-stream deferred flush contract
 */

#include <sstream>

#define private public
#include <galay/cpp/galay-http2/kernel/stream_manager.h>
#undef private

#include <cassert>
#include <iostream>

using namespace galay::http2;

int main() {
    Http2ActiveStreamBatch batch;
    auto stream = Http2Stream::create(7);

    batch.mark(stream, Http2StreamEvent::HeadersReady);
    batch.mark(stream, Http2StreamEvent::DataArrived | Http2StreamEvent::RequestComplete);

    auto ready = batch.take_ready();
    assert(ready.size() == 1);
    assert(ready[0]->stream_id() == 7);

    auto events = ready[0]->take_events();
    assert(has_http2_stream_event(events, Http2StreamEvent::HeadersReady));
    assert(has_http2_stream_event(events, Http2StreamEvent::DataArrived));
    assert(has_http2_stream_event(events, Http2StreamEvent::RequestComplete));

    auto empty = batch.take_ready();
    assert(empty.empty());

    std::cout << "T50-H2ActiveBatchFlush PASS\n";
    return 0;
}
