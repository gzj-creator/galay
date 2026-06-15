/**
 * @file T52-H2ActiveBatchMailbox.cc
 * @brief HTTP/2 active-connection direct batch mailbox contract
 */

#include <sstream>

#define private public
#include "galay-http2/kernel/stream_mgr.h"
#undef private

#include <cassert>
#include <iostream>
#include <vector>

using namespace galay::http2;

template<typename T>
concept HasBatchMailboxSurface = requires(T& mailbox, std::vector<Http2Stream::ptr>&& batch) {
    { mailbox.sendBatch(std::move(batch)) } -> std::same_as<void>;
    { mailbox.recvBatch(16) };
    { mailbox.close() } -> std::same_as<void>;
    { mailbox.reset() } -> std::same_as<void>;
};

int main() {
    static_assert(HasBatchMailboxSurface<Http2ActiveStreamMailbox>,
                  "Http2ActiveStreamMailbox must expose sendBatch/recvBatch/close/reset");

    Http2ActiveStreamMailbox mailbox;
    Http2ConnContext ctx(mailbox);

    auto first = Http2Stream::create(1);
    auto second = Http2Stream::create(3);
    first->m_pending_events = Http2StreamEvent::HeadersReady;
    second->m_pending_events = Http2StreamEvent::DataArrived | Http2StreamEvent::RequestComplete;

    std::vector<Http2Stream::ptr> ready;
    ready.push_back(first);
    ready.push_back(second);
    mailbox.sendBatch(std::move(ready));

    auto first_batch = ctx.getActiveStreams(1);
    assert(first_batch.await_ready());
    auto first_result = first_batch.await_resume();
    assert(first_result.has_value());
    assert(first_result->size() == 1);
    assert((*first_result)[0]->streamId() == 1);
    assert(hasHttp2StreamEvent((*first_result)[0]->takeEvents(), Http2StreamEvent::HeadersReady));

    auto second_batch = ctx.getActiveStreams(1);
    assert(second_batch.await_ready());
    auto second_result = second_batch.await_resume();
    assert(second_result.has_value());
    assert(second_result->size() == 1);
    assert((*second_result)[0]->streamId() == 3);
    const auto second_events = (*second_result)[0]->takeEvents();
    assert(hasHttp2StreamEvent(second_events, Http2StreamEvent::DataArrived));
    assert(hasHttp2StreamEvent(second_events, Http2StreamEvent::RequestComplete));

    mailbox.close();

    auto closed_batch = ctx.getActiveStreams(16);
    assert(closed_batch.await_ready());
    auto closed_result = closed_batch.await_resume();
    assert(!closed_result.has_value());

    std::cout << "T52-H2ActiveBatchMailbox PASS\n";
    return 0;
}
