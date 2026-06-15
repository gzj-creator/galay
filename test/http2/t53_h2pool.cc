/**
 * @file T53-H2StreamPool.cc
 * @brief HTTP/2 pooled stream reuse contract
 */

#include <sstream>

#define private public
#include "galay-http2/kernel/stream_mgr.h"
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

        first->setState(Http2StreamState::Closed);
        first->adjustSendWindow(-123);
        first->adjustRecvWindow(-321);
        first->setEndStreamReceived();
        first->setEndStreamSent();
        first->setEndHeadersReceived();
        first->appendHeaderBlock(std::string_view("abc"));
        first->setDecodedHeaders({{":method", "POST"}, {"x-test", "1"}});
        first->consumeDecodedHeadersAsRequest();
        first->appendRequestData(std::string_view("payload"));
        first->response().setStatus(503);
        first->response().setHeader("x-resp", "1");
        first->response().setBody("resp");
        first->m_pending_events = Http2StreamEvent::HeadersReady | Http2StreamEvent::RequestComplete;
        first->m_active_queued = true;
        first->setGoAwayError(Http2GoAwayError{
            .stream_id = 1,
            .last_stream_id = 1,
            .error_code = Http2ErrorCode::ProtocolError,
            .retryable = false,
            .debug = "boom",
        });
        first->closeFrameQueue();

        assert(first->waitRequestComplete().await_ready());
        assert(first->waitResponseComplete().await_ready());
    }

    assert(pool.available() == 1);

    auto reused = pool.acquire(3);
    assert(reused.get() == first_raw);
    assert(pool.available() == 0);
    assert(reused->streamId() == 3);
    assert(reused->state() == Http2StreamState::Idle);
    assert(reused->sendWindow() == static_cast<int32_t>(kDefaultInitialWindowSize));
    assert(reused->recvWindow() == static_cast<int32_t>(kDefaultInitialWindowSize));
    assert(!reused->isEndStreamReceived());
    assert(!reused->isEndStreamSent());
    assert(!reused->isEndHeadersReceived());
    assert(reused->headerBlock().empty());
    assert(!reused->hasDecodedHeaders());
    assert(reused->request().method.empty());
    assert(reused->request().scheme.empty());
    assert(reused->request().authority.empty());
    assert(reused->request().path.empty());
    assert(reused->request().headers.empty());
    assert(reused->request().bodySize() == 0);
    assert(reused->request().bodyChunkCount() == 0);
    assert(reused->response().status == 200);
    assert(reused->response().headers.empty());
    assert(reused->response().body.empty());
    assert(reused->takeEvents() == Http2StreamEvent::None);
    assert(!reused->hasGoAwayError());
    assert(!reused->isFrameQueueClosed());
    assert(reused->isFrameQueueEnabled());
    assert(!reused->waitRequestComplete().await_ready());
    assert(!reused->waitResponseComplete().await_ready());
    assert(!reused->getFrame().await_ready());

    galay::async::TcpSocket socket(GHandle{-1});
    Http2Conn conn(std::move(socket));
    Http2StreamManager manager(conn);
    manager.m_active_conn_mode = true;

    Http2Stream* manager_first_raw = nullptr;
    {
        auto manager_first = manager.createStreamInternal(1);
        manager_first_raw = manager_first.get();
        manager_first->setState(Http2StreamState::HalfClosedRemote);
        manager_first->onDataSent(true);
    }
    auto manager_reused = manager.createStreamInternal(3);
    assert(manager_reused.get() == manager_first_raw);
    assert(manager_reused->streamId() == 3);

    std::cout << "T53-H2StreamPool PASS\n";
    return 0;
}
