/**
 * @file t27_protocol_correctness.cc
 * @brief HTTP/2 protocol boundary regressions for flow control and frame sizes
 */

#include <array>
#include <cassert>
#include <iostream>
#include <string>
#include <vector>

#include <galay/cpp/galay-http2/builder/http2_frame_builder.h>
#include <sstream>
#define private public
#include <galay/cpp/galay-http2/kernel/stream_manager.h>
#undef private

using namespace galay::http2;
using namespace galay::async;

namespace
{

void require(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << message << std::endl;
        std::exit(1);
    }
}

std::string frame_bytes(Http2FrameType type,
                       uint8_t flags,
                       uint32_t stream_id,
                       std::string_view payload)
{
    std::string bytes(kHttp2FrameHeaderLength + payload.size(), '\0');
    Http2FrameHeader header;
    header.length = static_cast<uint32_t>(payload.size());
    header.type = type;
    header.flags = flags;
    header.stream_id = stream_id;
    header.serialize(reinterpret_cast<uint8_t*>(bytes.data()));
    bytes.replace(kHttp2FrameHeaderLength, payload.size(), payload);
    return bytes;
}

Http2DataFrame make_data(uint32_t stream_id, std::string payload, bool end_stream = false)
{
    Http2DataFrame frame;
    frame.header().stream_id = stream_id;
    frame.set_data(std::move(payload));
    frame.set_end_stream(end_stream);
    return frame;
}

Http2WindowUpdateFrame make_window_update(uint32_t stream_id, uint32_t increment)
{
    Http2WindowUpdateFrame frame;
    frame.header().stream_id = stream_id;
    frame.set_window_size_increment(increment);
    return frame;
}

void expect_pending_action(const std::deque<PendingAction>& actions,
                         PendingAction::Type type,
                         uint32_t stream_id,
                         Http2ErrorCode code)
{
    assert(!actions.empty());
    const auto& action = actions.back();
    assert(action.type == type);
    assert(action.stream_id == stream_id);
    assert(action.error_code == code);
}

void test_outbound_data_waits_for_stream_window()
{
    AsyncTcpSocket socket(GHandle{-1});
    Http2Conn conn(std::move(socket));
    Http2StreamManager manager(conn);

    auto stream = conn.create_stream(1);
    stream->set_state(Http2StreamState::Open);
    manager.attach_stream_io(stream);
    stream->adjust_send_window(-stream->send_window());

    auto wait = stream->reply_data(std::string("abc"), true);
    assert(!wait.m_waiter->is_ready());
    assert(manager.m_send_channel.empty());

    auto update = std::make_unique<Http2WindowUpdateFrame>(make_window_update(1, 3));
    manager.handle_window_update_frame(std::move(update), 1);

    auto sent = manager.m_send_channel.try_recv();
    assert(sent.has_value());
    assert(sent->flatten() == Http2FrameBuilder::data_bytes(1, "abc", true));
    assert(sent->waiter == wait.m_waiter);
    assert(!wait.m_waiter->is_ready());
    assert(stream->send_window() == 0);
    assert(stream->is_end_stream_sent());
}

void test_outbound_data_waits_for_connection_window()
{
    AsyncTcpSocket socket(GHandle{-1});
    Http2Conn conn(std::move(socket));
    Http2StreamManager manager(conn);

    auto stream = conn.create_stream(17);
    stream->set_state(Http2StreamState::Open);
    manager.attach_stream_io(stream);
    conn.adjust_conn_send_window(-conn.conn_send_window());

    auto wait = stream->reply_data(std::string("abc"), false);
    assert(!wait.m_waiter->is_ready());
    assert(manager.m_send_channel.empty());

    manager.handle_connection_frame(std::make_unique<Http2WindowUpdateFrame>(
        make_window_update(0, 3)));

    auto sent = manager.m_send_channel.try_recv();
    assert(sent.has_value());
    assert(sent->flatten() == Http2FrameBuilder::data_bytes(17, "abc", false));
    assert(sent->waiter == wait.m_waiter);
    assert(!wait.m_waiter->is_ready());
    assert(conn.conn_send_window() == 0);
    assert(stream->send_window() == static_cast<int32_t>(kDefaultInitialWindowSize - 3));
}

void test_pending_data_waiter_notified_on_close()
{
    auto stream = Http2Stream::create(3);
    std::vector<Http2OutgoingFrame> send_queue;
    stream->attach_io(&send_queue, nullptr, nullptr);
    stream->set_state(Http2StreamState::Open);
    stream->adjust_send_window(-stream->send_window());

    auto wait = stream->reply_data(std::string("blocked"), true);
    assert(!wait.m_waiter->is_ready());
    assert(send_queue.empty());

    stream->close_frame_queue();
    assert(wait.m_waiter->is_ready());
}

void test_trailers_do_not_spawn_second_handler()
{
    AsyncTcpSocket socket(GHandle{-1});
    Http2Conn conn(std::move(socket));
    Http2StreamManager manager(conn);
    auto stream = conn.create_stream(3);
    stream->on_headers_received(false);
    stream->set_decoded_headers({{":method", "POST"}, {":path", "/values/8"},
        {"content-type", "application/json"}});
    manager.complete_decoded_headers(stream, false);
    assert(manager.m_pending_spawns.size() == 1);
    assert(!stream->is_request_completed());
    stream->append_request_data(std::string("{}"));
    stream->on_headers_received(true);
    stream->set_decoded_headers({{"x-trailer", "complete"}});
    manager.complete_decoded_headers(stream, true);
    assert(stream->is_request_completed());
    assert(stream->request().get_header("x-trailer") == "complete");
    assert(manager.m_pending_spawns.size() == 1);
}

void test_incoming_data_checks_connection_recv_window_first()
{
    AsyncTcpSocket socket(GHandle{-1});
    Http2Conn conn(std::move(socket));
    Http2StreamManager manager(conn);
    auto stream = conn.create_stream(5);
    stream->set_state(Http2StreamState::Open);
    stream->adjust_recv_window(100);
    conn.adjust_conn_recv_window(-conn.conn_recv_window() + 1);

    manager.handle_data_frame(std::make_unique<Http2DataFrame>(make_data(5, "xx")), 5);

    expect_pending_action(manager.m_pending_actions,
                        PendingAction::Type::SendGoaway,
                        0,
                        Http2ErrorCode::FlowControlError);
    assert(conn.conn_recv_window() == 1);
    assert(stream->recv_window() > 1);
    assert(stream->request().body.empty());
}

void test_incoming_data_checks_stream_recv_window()
{
    AsyncTcpSocket socket(GHandle{-1});
    Http2Conn conn(std::move(socket));
    Http2StreamManager manager(conn);
    auto stream = conn.create_stream(7);
    stream->set_state(Http2StreamState::Open);
    stream->adjust_recv_window(-stream->recv_window() + 1);

    manager.handle_data_frame(std::make_unique<Http2DataFrame>(make_data(7, "xx")), 7);

    expect_pending_action(manager.m_pending_actions,
                        PendingAction::Type::SendRstStream,
                        7,
                        Http2ErrorCode::FlowControlError);
    assert(stream->recv_window() == 1);
    assert(stream->request().body.empty());
}

void test_window_update_overflow()
{
    AsyncTcpSocket socket(GHandle{-1});
    Http2Conn conn(std::move(socket));
    Http2StreamManager manager(conn);

    conn.adjust_conn_send_window(kMaxStreamId - conn.conn_send_window());
    manager.handle_connection_frame(std::make_unique<Http2WindowUpdateFrame>(
        make_window_update(0, 1)));

    expect_pending_action(manager.m_pending_actions,
                        PendingAction::Type::SendGoaway,
                        0,
                        Http2ErrorCode::FlowControlError);
    assert(conn.conn_send_window() == static_cast<int32_t>(kMaxStreamId));

    auto stream = conn.create_stream(9);
    stream->set_state(Http2StreamState::Open);
    stream->adjust_send_window(kMaxStreamId - stream->send_window());
    manager.handle_window_update_frame(std::make_unique<Http2WindowUpdateFrame>(
        make_window_update(9, 1)), 9);

    expect_pending_action(manager.m_pending_actions,
                        PendingAction::Type::SendRstStream,
                        9,
                        Http2ErrorCode::FlowControlError);
    assert(stream->send_window() == static_cast<int32_t>(kMaxStreamId));
}

void test_settings_initial_window_delta_applies_to_existing_streams()
{
    AsyncTcpSocket socket(GHandle{-1});
    Http2Conn conn(std::move(socket));
    Http2StreamManager manager(conn);

    auto stream1 = conn.create_stream(11);
    auto stream2 = conn.create_stream(13);
    stream1->adjust_send_window(-100);
    stream2->adjust_send_window(-200);

    Http2SettingsFrame settings;
    settings.add_setting(Http2SettingsId::InitialWindowSize,
                        kDefaultInitialWindowSize + 1000);
    manager.handle_connection_frame(std::make_unique<Http2SettingsFrame>(settings.clone()));

    assert(stream1->send_window() == static_cast<int32_t>(kDefaultInitialWindowSize + 900));
    assert(stream2->send_window() == static_cast<int32_t>(kDefaultInitialWindowSize + 800));

    stream1->adjust_send_window(kMaxStreamId - stream1->send_window());
    Http2SettingsFrame overflow;
    overflow.add_setting(Http2SettingsId::InitialWindowSize,
                        kDefaultInitialWindowSize + 1001);
    manager.handle_connection_frame(std::make_unique<Http2SettingsFrame>(overflow.clone()));

    expect_pending_action(manager.m_pending_actions,
                        PendingAction::Type::SendGoaway,
                        0,
                        Http2ErrorCode::FlowControlError);
    assert(conn.peer_settings().initial_window_size == kDefaultInitialWindowSize + 1000);
}

void test_new_streams_use_negotiated_windows()
{
    AsyncTcpSocket socket(GHandle{-1});
    Http2Conn conn(std::move(socket));
    Http2StreamPool pool;
    Http2SettingsFrame local;
    local.add_setting(Http2SettingsId::InitialWindowSize, 16384);
    assert(conn.apply_local_settings(local) == Http2ErrorCode::NoError);
    for (const uint32_t window : {0u, 4096u, 6291456u, kMaxStreamId}) {
        Http2SettingsFrame peer;
        peer.add_setting(Http2SettingsId::InitialWindowSize, window);
        assert(conn.apply_peer_settings(peer) == Http2ErrorCode::NoError);
        auto fresh = conn.create_stream(1);
        auto reused = conn.create_stream(3, pool.acquire(3));
        assert(fresh->send_window() == static_cast<int32_t>(window));
        assert(reused->send_window() == static_cast<int32_t>(window));
        assert(fresh->recv_window() == 16384 && reused->recv_window() == 16384);
        fresh->adjust_send_window(-1);
        assert(conn.create_stream(1) == fresh);
        assert(fresh->send_window() == static_cast<int32_t>(window) - 1);
        conn.remove_stream(1);
        conn.remove_stream(3);
    }
}

void test_unknown_extension_frame_is_consumed_and_ignored()
{
    AsyncTcpSocket socket(GHandle{-1});
    Http2Conn conn(std::move(socket));

    const auto unknown = frame_bytes(static_cast<Http2FrameType>(0x0b), 0, 1, "ext");
    const auto settings = frame_bytes(Http2FrameType::Settings, 0, 0, "");
    conn.feed_data(unknown.data(), unknown.size());
    conn.feed_data(settings.data(), settings.size());

    auto parsed = conn.parse_buffered_frames(4);
    assert(parsed.has_value());
    assert(parsed->size() == 1);
    assert(parsed->front()->is_settings());
    assert(conn.ring_buffer().readable() == 0);
}

void test_data_builder_splits_payload_at_default_max_frame_size()
{
    const std::string payload(kDefaultMaxFrameSize + 3, 'x');
    const auto bytes = Http2FrameBuilder::data_bytes(15, payload, true);

    AsyncTcpSocket socket(GHandle{-1});
    Http2Conn conn(std::move(socket));
    conn.feed_data(bytes.data(), bytes.size());

    auto parsed = conn.parse_buffered_frames(4);
    assert(parsed.has_value());
    assert(parsed->size() == 2);
    assert(parsed->at(0)->is_data());
    assert(parsed->at(0)->as_data()->data().size() == kDefaultMaxFrameSize);
    assert(!parsed->at(0)->as_data()->is_end_stream());
    assert(parsed->at(1)->is_data());
    assert(parsed->at(1)->as_data()->data().size() == 3);
    assert(parsed->at(1)->as_data()->is_end_stream());
}

void test_send_data_frame_rejects_insufficient_send_window()
{
    AsyncTcpSocket socket(GHandle{-1});
    Http2Conn conn(std::move(socket));
    auto stream = conn.create_stream(19);
    stream->set_state(Http2StreamState::Open);
    conn.adjust_conn_send_window(-conn.conn_send_window() + 1);

    const int32_t conn_window_before = conn.conn_send_window();
    const int32_t stream_window_before = stream->send_window();
    auto write = conn.send_data_frame(19, std::string("xx"), false);

    require(conn.conn_send_window() == conn_window_before,
            "connection window must not change on rejected DATA");
    require(stream->send_window() == stream_window_before,
            "stream window must not change on rejected DATA");
    require(write.await_ready(), "flow-control rejection must be an immediate awaitable");
    auto result = write.await_resume();
    require(!result.has_value(), "flow-control rejection must return an error");
    require(result.error() == Http2ErrorCode::FlowControlError,
            "flow-control rejection must preserve FlowControlError");
}

} // namespace

int main()
{
    test_outbound_data_waits_for_stream_window();
    test_outbound_data_waits_for_connection_window();
    test_pending_data_waiter_notified_on_close();
    test_trailers_do_not_spawn_second_handler();
    test_incoming_data_checks_connection_recv_window_first();
    test_incoming_data_checks_stream_recv_window();
    test_window_update_overflow();
    test_settings_initial_window_delta_applies_to_existing_streams();
    test_new_streams_use_negotiated_windows();
    test_unknown_extension_frame_is_consumed_and_ignored();
    test_data_builder_splits_payload_at_default_max_frame_size();
    test_send_data_frame_rejects_insufficient_send_window();

    std::cout << "t27_protocol_correctness PASS\n";
    return 0;
}
