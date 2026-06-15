/**
 * @file T50-H2ActiveBatchFlush.cc
 * @brief HTTP/2 active-stream deferred flush contract
 */

#include <sstream>

#define private public
#include "galay-http2/kernel/stream_mgr.h"
#undef private

#include <cassert>
#include <iostream>

using namespace galay::http2;

int main() {
    Http2ActiveStreamBatch batch;
    auto stream = Http2Stream::create(7);

    batch.mark(stream, Http2StreamEvent::HeadersReady);
    batch.mark(stream, Http2StreamEvent::DataArrived | Http2StreamEvent::RequestComplete);

    auto ready = batch.takeReady();
    assert(ready.size() == 1);
    assert(ready[0]->streamId() == 7);

    auto events = ready[0]->takeEvents();
    assert(hasHttp2StreamEvent(events, Http2StreamEvent::HeadersReady));
    assert(hasHttp2StreamEvent(events, Http2StreamEvent::DataArrived));
    assert(hasHttp2StreamEvent(events, Http2StreamEvent::RequestComplete));

    auto empty = batch.takeReady();
    assert(empty.empty());

    std::cout << "T50-H2ActiveBatchFlush PASS\n";
    return 0;
}
