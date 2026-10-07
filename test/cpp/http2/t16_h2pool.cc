/**
 * @file T53-H2StreamPool.cc
 * @brief HTTP/2 pooled stream reuse contract
 */

#include <sstream>

#define private public
#include <galay/cpp/galay-http2/kernel/stream_manager.h>
#undef private

#include <cassert>
#include <iostream>

using namespace galay::http2;

template<typename T>
concept HasStreamPoolSurface = requires(T& pool) {
    { pool.acquire(1) } -> std::same_as<Http2Stream::ptr>;
    { pool.available() } -> std::same_as<size_t>;
};

int main() {
    static_assert(HasStreamPoolSurface<Http2StreamPool>,
                  "Http2StreamPool must expose acquire(stream_id) and available()");

    Http2StreamPool pool;
    Http2Stream* first_raw = nullptr;

    {
        auto first = pool.acquire(1);
        first_raw = first.get();

        first->set_state(Http2StreamState::Closed);
        first->adjust_send_window(-123);
        first->adjust_recv_window(-321);
        first->set_end_stream_received();
        first->set_end_stream_sent();
        first->set_end_headers_received();
        first->append_header_block(std::string_view("abc"));
        first->set_decoded_headers({{":method", "POST"}, {"x-test", "1"}});
        first->consume_decoded_headers_as_request();
        first->append_request_data(std::string_view("payload"));
        first->response().set_status(503);
        first->response().set_header("x-resp", "1");
        first->response().set_body("resp");
        first->m_pending_events = Http2StreamEvent::HeadersReady | Http2StreamEvent::RequestComplete;
        first->m_active_queued = true;
        first->set_go_away_error(Http2GoAwayError{
            .stream_id = 1,
            .last_stream_id = 1,
            .error_code = Http2ErrorCode::ProtocolError,
            .retryable = false,
            .debug = "boom",
        });
        first->close_frame_queue();

        assert(first->wait_request_complete().await_ready());
        assert(first->wait_response_complete().await_ready());
    }

    assert(pool.available() == 1);

    auto reused = pool.acquire(3);
    assert(reused.get() == first_raw);
    assert(pool.available() == 0);
    assert(reused->stream_id() == 3);
    assert(reused->state() == Http2StreamState::Idle);
    assert(reused->send_window() == static_cast<int32_t>(kDefaultInitialWindowSize));
    assert(reused->recv_window() == static_cast<int32_t>(kDefaultInitialWindowSize));
    assert(!reused->is_end_stream_received());
    assert(!reused->is_end_stream_sent());
    assert(!reused->is_end_headers_received());
    assert(reused->header_block().empty());
    assert(!reused->has_decoded_headers());
    assert(reused->request().method.empty());
    assert(reused->request().scheme.empty());
    assert(reused->request().authority.empty());
    assert(reused->request().path.empty());
    assert(reused->request().headers.empty());
    assert(reused->request().body_size() == 0);
    assert(reused->request().body_chunk_count() == 0);
    assert(reused->response().status == 200);
    assert(reused->response().headers.empty());
    assert(reused->response().body.empty());
    assert(reused->take_events() == Http2StreamEvent::None);
    assert(!reused->has_go_away_error());
    assert(!reused->is_frame_queue_closed());
    assert(reused->is_frame_queue_enabled());
    assert(!reused->wait_request_complete().await_ready());
    assert(!reused->wait_response_complete().await_ready());
    assert(!reused->get_frame().await_ready());

    galay::async::AsyncTcpSocket socket(GHandle{-1});
    Http2Conn conn(std::move(socket));
    Http2StreamManager manager(conn);
    manager.m_active_conn_mode = true;

    Http2Stream* manager_first_raw = nullptr;
    {
        auto manager_first = manager.create_stream_internal(1);
        manager_first_raw = manager_first.get();
        manager_first->set_state(Http2StreamState::HalfClosedRemote);
        manager_first->on_data_sent(true);
    }
    auto manager_reused = manager.create_stream_internal(3);
    assert(manager_reused.get() == manager_first_raw);
    assert(manager_reused->stream_id() == 3);

    std::cout << "T53-H2StreamPool PASS\n";
    return 0;
}
