/**
 * @file T77-H2ActiveConnDeferKnownBodyHeaders.cc
 * @brief HTTP/2 active-connection mode should defer headers-only delivery for known non-zero request bodies
 */

#include <sstream>

#define private public
#include "galay-http2/kernel/stream_mgr.h"
#undef private

#include <cassert>
#include <iostream>

using namespace galay::http2;

namespace {

Http2Stream::ptr makeDecodedRequest(Http2StreamManager& manager,
                                    uint32_t stream_id,
                                    size_t content_length) {
    auto stream = manager.createStreamInternal(stream_id);
    Http2Headers headers;
    headers.method("POST")
        .scheme("https")
        .authority("127.0.0.1:9443")
        .path("/echo")
        .contentType("text/plain")
        .contentLength(content_length);
    stream->appendHeaderBlock(manager.conn().encoder().encode(headers.fields()));
    return stream;
}

} // namespace

int main() {
    galay::async::TcpSocket socket(GHandle{-1});
    Http2Conn conn(std::move(socket));
    Http2StreamManager manager(conn);
    manager.m_active_conn_mode = true;

    {
        auto stream = makeDecodedRequest(manager, 1, 128);
        manager.completeReceivedHeaders(stream, false);

        if (!manager.m_active_batch.empty()) {
            std::cerr << "[T77] headers-only delivery for known non-zero request body should stay deferred\n";
            return 1;
        }
        if (!hasHttp2StreamEvent(stream->m_pending_events, Http2StreamEvent::HeadersReady)) {
            std::cerr << "[T77] deferred stream should still retain HeadersReady in pending events\n";
            return 1;
        }
        if (stream->m_active_queued) {
            std::cerr << "[T77] deferred headers-only stream should not be queued yet\n";
            return 1;
        }
    }

    {
        auto stream = makeDecodedRequest(manager, 3, 0);
        manager.completeReceivedHeaders(stream, false);

        if (manager.m_active_batch.empty()) {
            std::cerr << "[T77] zero-body request should still be delivered immediately on headers\n";
            return 1;
        }
        auto ready = manager.m_active_batch.takeReady();
        if (ready.size() != 1 || ready[0]->streamId() != 3) {
            std::cerr << "[T77] zero-body request should enqueue exactly one ready stream\n";
            return 1;
        }
        auto events = ready[0]->takeEvents();
        if (!hasHttp2StreamEvent(events, Http2StreamEvent::HeadersReady)) {
            std::cerr << "[T77] immediate delivery should preserve HeadersReady event\n";
            return 1;
        }
    }

    std::cout << "T77-H2ActiveConnDeferKnownBodyHeaders PASS\n";
    return 0;
}
