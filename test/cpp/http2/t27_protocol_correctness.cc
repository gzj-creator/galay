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

std::string frameBytes(Http2FrameType type,
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

Http2DataFrame makeData(uint32_t stream_id, std::string payload, bool end_stream = false)
{
    Http2DataFrame frame;
    frame.header().stream_id = stream_id;
    frame.setData(std::move(payload));
    frame.setEndStream(end_stream);
    return frame;
}

Http2WindowUpdateFrame makeWindowUpdate(uint32_t stream_id, uint32_t increment)
{
    Http2WindowUpdateFrame frame;
    frame.header().stream_id = stream_id;
    frame.setWindowSizeIncrement(increment);
    return frame;
}

void expectPendingAction(const std::deque<PendingAction>& actions,
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

void testOutboundDataWaitsForStreamWindow()
{
    TcpSocket socket(GHandle{-1});
    Http2Conn conn(std::move(socket));
    Http2StreamManager manager(conn);

    auto stream = conn.createStream(1);
    stream->setState(Http2StreamState::Open);
    manager.attachStreamIO(stream);
    stream->adjustSendWindow(-stream->sendWindow());

    auto wait = stream->replyData(std::string("abc"), true);
    assert(!wait.m_waiter->isReady());
    assert(manager.m_send_channel.empty());

    auto update = std::make_unique<Http2WindowUpdateFrame>(makeWindowUpdate(1, 3));
    manager.handleWindowUpdateFrame(std::move(update), 1);

    auto sent = manager.m_send_channel.tryRecv();
    assert(sent.has_value());
    assert(sent->flatten() == Http2FrameBuilder::dataBytes(1, "abc", true));
    assert(sent->waiter == wait.m_waiter);
    assert(!wait.m_waiter->isReady());
    assert(stream->sendWindow() == 0);
    assert(stream->isEndStreamSent());
}

void testOutboundDataWaitsForConnectionWindow()
{
    TcpSocket socket(GHandle{-1});
    Http2Conn conn(std::move(socket));
    Http2StreamManager manager(conn);

    auto stream = conn.createStream(17);
    stream->setState(Http2StreamState::Open);
    manager.attachStreamIO(stream);
    conn.adjustConnSendWindow(-conn.connSendWindow());

    auto wait = stream->replyData(std::string("abc"), false);
    assert(!wait.m_waiter->isReady());
    assert(manager.m_send_channel.empty());

    manager.handleConnectionFrame(std::make_unique<Http2WindowUpdateFrame>(
        makeWindowUpdate(0, 3)));

    auto sent = manager.m_send_channel.tryRecv();
    assert(sent.has_value());
    assert(sent->flatten() == Http2FrameBuilder::dataBytes(17, "abc", false));
    assert(sent->waiter == wait.m_waiter);
    assert(!wait.m_waiter->isReady());
    assert(conn.connSendWindow() == 0);
    assert(stream->sendWindow() == static_cast<int32_t>(kDefaultInitialWindowSize - 3));
}

void testPendingDataWaiterNotifiedOnClose()
{
    auto stream = Http2Stream::create(3);
    std::vector<Http2OutgoingFrame> send_queue;
    stream->attachIO(&send_queue, nullptr, nullptr);
    stream->setState(Http2StreamState::Open);
    stream->adjustSendWindow(-stream->sendWindow());

    auto wait = stream->replyData(std::string("blocked"), true);
    assert(!wait.m_waiter->isReady());
    assert(send_queue.empty());

    stream->closeFrameQueue();
    assert(wait.m_waiter->isReady());
}

void testIncomingDataChecksConnectionRecvWindowFirst()
{
    TcpSocket socket(GHandle{-1});
    Http2Conn conn(std::move(socket));
    Http2StreamManager manager(conn);
    auto stream = conn.createStream(5);
    stream->setState(Http2StreamState::Open);
    stream->adjustRecvWindow(100);
    conn.adjustConnRecvWindow(-conn.connRecvWindow() + 1);

    manager.handleDataFrame(std::make_unique<Http2DataFrame>(makeData(5, "xx")), 5);

    expectPendingAction(manager.m_pending_actions,
                        PendingAction::Type::SendGoaway,
                        0,
                        Http2ErrorCode::FlowControlError);
    assert(conn.connRecvWindow() == 1);
    assert(stream->recvWindow() > 1);
    assert(stream->request().body.empty());
}

void testIncomingDataChecksStreamRecvWindow()
{
    TcpSocket socket(GHandle{-1});
    Http2Conn conn(std::move(socket));
    Http2StreamManager manager(conn);
    auto stream = conn.createStream(7);
    stream->setState(Http2StreamState::Open);
    stream->adjustRecvWindow(-stream->recvWindow() + 1);

    manager.handleDataFrame(std::make_unique<Http2DataFrame>(makeData(7, "xx")), 7);

    expectPendingAction(manager.m_pending_actions,
                        PendingAction::Type::SendRstStream,
                        7,
                        Http2ErrorCode::FlowControlError);
    assert(stream->recvWindow() == 1);
    assert(stream->request().body.empty());
}

void testWindowUpdateOverflow()
{
    TcpSocket socket(GHandle{-1});
    Http2Conn conn(std::move(socket));
    Http2StreamManager manager(conn);

    conn.adjustConnSendWindow(kMaxStreamId - conn.connSendWindow());
    manager.handleConnectionFrame(std::make_unique<Http2WindowUpdateFrame>(
        makeWindowUpdate(0, 1)));

    expectPendingAction(manager.m_pending_actions,
                        PendingAction::Type::SendGoaway,
                        0,
                        Http2ErrorCode::FlowControlError);
    assert(conn.connSendWindow() == static_cast<int32_t>(kMaxStreamId));

    auto stream = conn.createStream(9);
    stream->setState(Http2StreamState::Open);
    stream->adjustSendWindow(kMaxStreamId - stream->sendWindow());
    manager.handleWindowUpdateFrame(std::make_unique<Http2WindowUpdateFrame>(
        makeWindowUpdate(9, 1)), 9);

    expectPendingAction(manager.m_pending_actions,
                        PendingAction::Type::SendRstStream,
                        9,
                        Http2ErrorCode::FlowControlError);
    assert(stream->sendWindow() == static_cast<int32_t>(kMaxStreamId));
}

void testSettingsInitialWindowDeltaAppliesToExistingStreams()
{
    TcpSocket socket(GHandle{-1});
    Http2Conn conn(std::move(socket));
    Http2StreamManager manager(conn);

    auto stream1 = conn.createStream(11);
    auto stream2 = conn.createStream(13);
    stream1->adjustSendWindow(-100);
    stream2->adjustSendWindow(-200);

    Http2SettingsFrame settings;
    settings.addSetting(Http2SettingsId::InitialWindowSize,
                        kDefaultInitialWindowSize + 1000);
    manager.handleConnectionFrame(std::make_unique<Http2SettingsFrame>(settings));

    assert(stream1->sendWindow() == static_cast<int32_t>(kDefaultInitialWindowSize + 900));
    assert(stream2->sendWindow() == static_cast<int32_t>(kDefaultInitialWindowSize + 800));

    stream1->adjustSendWindow(kMaxStreamId - stream1->sendWindow());
    Http2SettingsFrame overflow;
    overflow.addSetting(Http2SettingsId::InitialWindowSize,
                        kDefaultInitialWindowSize + 1001);
    manager.handleConnectionFrame(std::make_unique<Http2SettingsFrame>(overflow));

    expectPendingAction(manager.m_pending_actions,
                        PendingAction::Type::SendGoaway,
                        0,
                        Http2ErrorCode::FlowControlError);
    assert(conn.peerSettings().initial_window_size == kDefaultInitialWindowSize + 1000);
}

void testUnknownExtensionFrameIsConsumedAndIgnored()
{
    TcpSocket socket(GHandle{-1});
    Http2Conn conn(std::move(socket));

    const auto unknown = frameBytes(static_cast<Http2FrameType>(0x0b), 0, 1, "ext");
    const auto settings = frameBytes(Http2FrameType::Settings, 0, 0, "");
    conn.feedData(unknown.data(), unknown.size());
    conn.feedData(settings.data(), settings.size());

    auto parsed = conn.parseBufferedFrames(4);
    assert(parsed.has_value());
    assert(parsed->size() == 1);
    assert(parsed->front()->isSettings());
    assert(conn.ringBuffer().readable() == 0);
}

void testDataBuilderSplitsPayloadAtDefaultMaxFrameSize()
{
    const std::string payload(kDefaultMaxFrameSize + 3, 'x');
    const auto bytes = Http2FrameBuilder::dataBytes(15, payload, true);

    TcpSocket socket(GHandle{-1});
    Http2Conn conn(std::move(socket));
    conn.feedData(bytes.data(), bytes.size());

    auto parsed = conn.parseBufferedFrames(4);
    assert(parsed.has_value());
    assert(parsed->size() == 2);
    assert(parsed->at(0)->isData());
    assert(parsed->at(0)->asData()->data().size() == kDefaultMaxFrameSize);
    assert(!parsed->at(0)->asData()->isEndStream());
    assert(parsed->at(1)->isData());
    assert(parsed->at(1)->asData()->data().size() == 3);
    assert(parsed->at(1)->asData()->isEndStream());
}

void testSendDataFrameRejectsInsufficientSendWindow()
{
    TcpSocket socket(GHandle{-1});
    Http2Conn conn(std::move(socket));
    auto stream = conn.createStream(19);
    stream->setState(Http2StreamState::Open);
    conn.adjustConnSendWindow(-conn.connSendWindow() + 1);

    const int32_t conn_window_before = conn.connSendWindow();
    const int32_t stream_window_before = stream->sendWindow();
    auto write = conn.sendDataFrame(19, std::string("xx"), false);

    require(conn.connSendWindow() == conn_window_before,
            "connection window must not change on rejected DATA");
    require(stream->sendWindow() == stream_window_before,
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
    testOutboundDataWaitsForStreamWindow();
    testOutboundDataWaitsForConnectionWindow();
    testPendingDataWaiterNotifiedOnClose();
    testIncomingDataChecksConnectionRecvWindowFirst();
    testIncomingDataChecksStreamRecvWindow();
    testWindowUpdateOverflow();
    testSettingsInitialWindowDeltaAppliesToExistingStreams();
    testUnknownExtensionFrameIsConsumedAndIgnored();
    testDataBuilderSplitsPayloadAtDefaultMaxFrameSize();
    testSendDataFrameRejectsInsufficientSendWindow();

    std::cout << "t27_protocol_correctness PASS\n";
    return 0;
}
