/**
 * @file T77-H2ActiveConnDeferKnownBodyHeaders.cc
 * @brief HTTP/2 active-connection mode should defer headers-only delivery for known non-zero request bodies
 */

#include <sstream>

#define private public
#include <galay/cpp/galay-http2/kernel/stream_manager.h>
#undef private

#include <cassert>
#include <iostream>

using namespace galay::http2;

namespace {

Http2Stream::ptr make_decoded_request(Http2StreamManager& manager,
                                    uint32_t stream_id,
                                    size_t content_length) {
    auto stream = manager.create_stream_internal(stream_id);
    Http2Headers headers;
    headers.method("POST")
        .scheme("https")
        .authority("127.0.0.1:9443")
        .path("/echo")
        .content_type("text/plain")
        .content_length(content_length);
    stream->append_header_block(manager.conn().encoder().encode(headers.fields()));
    return stream;
}

} // namespace

int main() {
    galay::async::AsyncTcpSocket socket(GHandle{-1});
    Http2Conn conn(std::move(socket));
    Http2StreamManager manager(conn);
    manager.m_active_conn_mode = true;

    {
        auto stream = make_decoded_request(manager, 1, 128);
        manager.complete_received_headers(stream, false);

        if (!manager.m_active_batch.empty()) {
            std::cerr << "[T77] headers-only delivery for known non-zero request body should stay deferred\n";
            return 1;
        }
        if (!has_http2_stream_event(stream->m_pending_events, Http2StreamEvent::HeadersReady)) {
            std::cerr << "[T77] deferred stream should still retain HeadersReady in pending events\n";
            return 1;
        }
        if (stream->m_active_queued) {
            std::cerr << "[T77] deferred headers-only stream should not be queued yet\n";
            return 1;
        }
    }

    {
        auto stream = make_decoded_request(manager, 3, 0);
        manager.complete_received_headers(stream, false);

        if (manager.m_active_batch.empty()) {
            std::cerr << "[T77] zero-body request should still be delivered immediately on headers\n";
            return 1;
        }
        auto ready = manager.m_active_batch.take_ready();
        if (ready.size() != 1 || ready[0]->stream_id() != 3) {
            std::cerr << "[T77] zero-body request should enqueue exactly one ready stream\n";
            return 1;
        }
        auto events = ready[0]->take_events();
        if (!has_http2_stream_event(events, Http2StreamEvent::HeadersReady)) {
            std::cerr << "[T77] immediate delivery should preserve HeadersReady event\n";
            return 1;
        }
    }

    std::cout << "T77-H2ActiveConnDeferKnownBodyHeaders PASS\n";
    return 0;
}
