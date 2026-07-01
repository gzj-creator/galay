#include <chrono>
#include <sstream>

#define private public
#include <galay/cpp/galay-http2/kernel/stream_manager.h>
#undef private

#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

using namespace galay::http2;

namespace {

bool check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        return false;
    }
    return true;
}

Http2SettingsFrame settingsFrame(Http2SettingsId id, uint32_t value) {
    Http2SettingsFrame frame;
    frame.addSetting(id, value);
    return frame;
}

std::vector<uint8_t> dataFrameBytes(uint32_t stream_id, uint32_t payload_len) {
    std::vector<uint8_t> frame(kHttp2FrameHeaderLength + payload_len);
    frame[0] = static_cast<uint8_t>((payload_len >> 16) & 0xff);
    frame[1] = static_cast<uint8_t>((payload_len >> 8) & 0xff);
    frame[2] = static_cast<uint8_t>(payload_len & 0xff);
    frame[3] = static_cast<uint8_t>(Http2FrameType::Data);
    frame[4] = 0;
    frame[5] = static_cast<uint8_t>((stream_id >> 24) & 0x7f);
    frame[6] = static_cast<uint8_t>((stream_id >> 16) & 0xff);
    frame[7] = static_cast<uint8_t>((stream_id >> 8) & 0xff);
    frame[8] = static_cast<uint8_t>(stream_id & 0xff);
    return frame;
}

uint32_t framePayloadLength(const std::string& bytes) {
    return (static_cast<uint32_t>(static_cast<uint8_t>(bytes[0])) << 16) |
           (static_cast<uint32_t>(static_cast<uint8_t>(bytes[1])) << 8) |
           static_cast<uint32_t>(static_cast<uint8_t>(bytes[2]));
}

uint8_t frameType(const std::string& bytes) {
    return static_cast<uint8_t>(bytes[3]);
}

uint8_t frameFlags(const std::string& bytes) {
    return static_cast<uint8_t>(bytes[4]);
}

uint32_t frameStreamId(const std::string& bytes) {
    return ((static_cast<uint32_t>(static_cast<uint8_t>(bytes[5])) & 0x7f) << 24) |
           (static_cast<uint32_t>(static_cast<uint8_t>(bytes[6])) << 16) |
           (static_cast<uint32_t>(static_cast<uint8_t>(bytes[7])) << 8) |
           static_cast<uint32_t>(static_cast<uint8_t>(bytes[8]));
}

bool assertInboundUsesLocalMaxFrameSizeForRejection() {
    bool ok = true;
    galay::async::TcpSocket socket(GHandle{-1});
    Http2Conn conn(std::move(socket));

    ok &= check(conn.applyLocalSettings(
        settingsFrame(Http2SettingsId::MaxFrameSize, kMinFrameSize)) == Http2ErrorCode::NoError,
        "local MAX_FRAME_SIZE setup must succeed");
    ok &= check(conn.applyPeerSettings(
        settingsFrame(Http2SettingsId::MaxFrameSize, kMinFrameSize + 4096)) == Http2ErrorCode::NoError,
        "peer MAX_FRAME_SIZE setup must succeed");

    auto frame = dataFrameBytes(1, kMinFrameSize + 1);
    conn.feedData(reinterpret_cast<const char*>(frame.data()), kHttp2FrameHeaderLength);
    auto result = conn.parseBufferedFrames();

    ok &= check(!result.has_value(),
                "inbound DATA larger than local MAX_FRAME_SIZE must be rejected");
    ok &= check(!result.has_value() && result.error() == Http2ErrorCode::FrameSizeError,
                "local inbound MAX_FRAME_SIZE rejection must be FrameSizeError");
    return ok;
}

bool assertInboundIgnoresPeerMaxFrameSizeForAcceptance() {
    bool ok = true;
    galay::async::TcpSocket socket(GHandle{-1});
    Http2Conn conn(std::move(socket));

    ok &= check(conn.applyLocalSettings(
        settingsFrame(Http2SettingsId::MaxFrameSize, kMinFrameSize + 4096)) ==
            Http2ErrorCode::NoError,
        "larger local MAX_FRAME_SIZE setup must succeed");
    ok &= check(conn.applyPeerSettings(
        settingsFrame(Http2SettingsId::MaxFrameSize, kMinFrameSize)) == Http2ErrorCode::NoError,
        "smaller peer MAX_FRAME_SIZE setup must succeed");

    auto frame = dataFrameBytes(1, kMinFrameSize + 1);
    conn.feedData(reinterpret_cast<const char*>(frame.data()), frame.size());
    auto result = conn.parseBufferedFrames();

    ok &= check(result.has_value(),
                "inbound DATA within local MAX_FRAME_SIZE must parse even if larger than peer limit");
    ok &= check(result.has_value() && result->size() == 1,
                "accepted inbound DATA must return one parsed frame");
    ok &= check(result.has_value() && !result->empty() &&
                    result->front()->type() == Http2FrameType::Data,
                "accepted inbound frame must remain DATA");
    return ok;
}

bool assertOutboundDataSplitsByPeerMaxFrameSize() {
    bool ok = true;
    galay::async::TcpSocket socket(GHandle{-1});
    Http2Conn conn(std::move(socket));

    ok &= check(conn.applyPeerSettings(
        settingsFrame(Http2SettingsId::MaxFrameSize, kMinFrameSize)) == Http2ErrorCode::NoError,
        "peer MAX_FRAME_SIZE setup for outbound split must succeed");

    std::vector<Http2OutgoingFrame> send_queue;
    auto stream = conn.createStream(1);
    stream->attachIO(&send_queue, &conn.encoder(), &conn.decoder(), nullptr, conn.peerSettings().max_frame_size);
    std::string payload(kMinFrameSize + 1, 'x');
    stream->sendData(payload, true);

    ok &= check(send_queue.size() >= 1,
                "outbound DATA larger than peer MAX_FRAME_SIZE must enqueue first fragment");
    ok &= check(send_queue.size() >= 2,
                "outbound DATA larger than peer MAX_FRAME_SIZE must enqueue trailing fragment");
    ok &= check(send_queue.size() == 2,
                "outbound DATA peer MAX_FRAME_SIZE split must not enqueue extra fragments");
    ok &= check(send_queue.size() == 2 &&
                    !send_queue[0].isEmpty() && !send_queue[1].isEmpty(),
                "outbound split fragments must contain serialized bytes");

    if (send_queue.size() == 2 && !send_queue[0].isEmpty() && !send_queue[1].isEmpty()) {
        const auto first_bytes = send_queue[0].flatten();
        const auto second_bytes = send_queue[1].flatten();

        ok &= check(frameType(first_bytes) == static_cast<uint8_t>(Http2FrameType::Data) &&
                        frameType(second_bytes) == static_cast<uint8_t>(Http2FrameType::Data),
                    "outbound split fragments must both be DATA frames");
        ok &= check(framePayloadLength(first_bytes) == kMinFrameSize,
                    "first outbound DATA fragment must match peer MAX_FRAME_SIZE");
        ok &= check(framePayloadLength(second_bytes) == 1,
                    "second outbound DATA fragment must carry the remainder");
        ok &= check((frameFlags(first_bytes) & Http2FrameFlags::kEndStream) == 0,
                    "non-terminal outbound DATA fragment must not carry END_STREAM");
        ok &= check((frameFlags(second_bytes) & Http2FrameFlags::kEndStream) != 0,
                    "final outbound DATA fragment must carry END_STREAM");
    }

    return ok;
}

bool assertOutboundHeadersSplitByPeerMaxFrameSize() {
    bool ok = true;
    galay::async::TcpSocket socket(GHandle{-1});
    Http2Conn conn(std::move(socket));
    Http2StreamManager manager(conn);

    ok &= check(conn.applyPeerSettings(
        settingsFrame(Http2SettingsId::MaxFrameSize, kMinFrameSize)) == Http2ErrorCode::NoError,
        "peer MAX_FRAME_SIZE setup for outbound HEADERS split must succeed");
    ok &= check(conn.applyPeerSettings(
        settingsFrame(Http2SettingsId::MaxHeaderListSize, kMinFrameSize * 3)) ==
            Http2ErrorCode::NoError,
        "peer MAX_HEADER_LIST_SIZE setup for outbound HEADERS split must succeed");

    auto stream = conn.createStream(3);
    manager.attachStreamIO(stream);

    const std::string header_block(kMinFrameSize * 2 + 7, 'h');
    stream->sendEncodedHeaders(header_block, false, true);

    auto outgoing = manager.m_send_channel.tryRecvBatch(8);
    ok &= check(outgoing.has_value(),
                "oversized outbound HEADERS must enqueue a fragmented batch");
    ok &= check(outgoing && outgoing->size() == 3,
                "oversized outbound HEADERS must split into HEADERS plus CONTINUATION frames");

    if (outgoing && outgoing->size() == 3) {
        const auto first = outgoing->at(0).flatten();
        const auto second = outgoing->at(1).flatten();
        const auto third = outgoing->at(2).flatten();

        ok &= check(frameType(first) == static_cast<uint8_t>(Http2FrameType::Headers),
                    "first outbound header fragment must be HEADERS");
        ok &= check(frameType(second) == static_cast<uint8_t>(Http2FrameType::Continuation) &&
                        frameType(third) == static_cast<uint8_t>(Http2FrameType::Continuation),
                    "trailing outbound header fragments must be CONTINUATION");
        ok &= check(framePayloadLength(first) == kMinFrameSize &&
                        framePayloadLength(second) == kMinFrameSize &&
                        framePayloadLength(third) == 7,
                    "outbound HEADERS fragments must obey peer MAX_FRAME_SIZE");
        ok &= check(frameStreamId(first) == 3 &&
                        frameStreamId(second) == 3 &&
                        frameStreamId(third) == 3,
                    "all outbound HEADERS fragments must stay on the same stream");
        ok &= check((frameFlags(first) & Http2FrameFlags::kEndHeaders) == 0 &&
                        (frameFlags(second) & Http2FrameFlags::kEndHeaders) == 0,
                    "non-final outbound header fragments must not carry END_HEADERS");
        ok &= check((frameFlags(third) & Http2FrameFlags::kEndHeaders) != 0,
                    "final outbound header fragment must carry END_HEADERS");
        ok &= check((frameFlags(first) & Http2FrameFlags::kEndStream) == 0 &&
                        (frameFlags(second) & Http2FrameFlags::kEndStream) == 0 &&
                        (frameFlags(third) & Http2FrameFlags::kEndStream) == 0,
                    "header-only fragmented send must not carry END_STREAM");
    }

    return ok;
}

bool assertOutboundHeaderBlockCapRejectsOversize() {
    bool ok = true;
    galay::async::TcpSocket socket(GHandle{-1});
    Http2Conn conn(std::move(socket));
    Http2StreamManager manager(conn);

    ok &= check(conn.applyPeerSettings(
        settingsFrame(Http2SettingsId::MaxHeaderListSize, 64)) == Http2ErrorCode::NoError,
        "peer MAX_HEADER_LIST_SIZE setup for outbound cap must succeed");

    auto stream = conn.createStream(5);
    manager.attachStreamIO(stream);

    stream->sendEncodedHeaders(std::string(65, 'c'), false, true);

    ok &= check(!manager.m_send_channel.tryRecv().has_value(),
                "outbound header block above peer MAX_HEADER_LIST_SIZE must not be enqueued");
    ok &= check(stream->state() == Http2StreamState::Idle,
                "rejected outbound header block must not advance stream state");
    ok &= check(!stream->isEndStreamSent(),
                "rejected outbound header block must not mark END_STREAM sent");
    return ok;
}

bool assertOutboundCombinedHeadersAndDataRespectPeerLimits() {
    bool ok = true;
    galay::async::TcpSocket socket(GHandle{-1});
    Http2Conn conn(std::move(socket));
    Http2StreamManager manager(conn);

    ok &= check(conn.applyPeerSettings(
        settingsFrame(Http2SettingsId::MaxFrameSize, kMinFrameSize)) == Http2ErrorCode::NoError,
        "peer MAX_FRAME_SIZE setup for outbound combined split must succeed");
    ok &= check(conn.applyPeerSettings(
        settingsFrame(Http2SettingsId::MaxHeaderListSize, kMinFrameSize * 3)) ==
            Http2ErrorCode::NoError,
        "peer MAX_HEADER_LIST_SIZE setup for outbound combined split must succeed");

    auto stream = conn.createStream(7);
    manager.attachStreamIO(stream);

    stream->sendEncodedHeadersAndData(
        std::string(kMinFrameSize * 2 + 7, 'h'),
        std::string(kMinFrameSize + 1, 'd'),
        true);

    auto outgoing = manager.m_send_channel.tryRecvBatch(8);
    ok &= check(outgoing.has_value(),
                "oversized outbound combined HEADERS+DATA must enqueue a fragmented batch");
    ok &= check(outgoing && outgoing->size() == 5,
                "oversized outbound combined HEADERS+DATA must split both HEADERS and DATA");

    if (outgoing && outgoing->size() == 5) {
        const auto first = outgoing->at(0).flatten();
        const auto second = outgoing->at(1).flatten();
        const auto third = outgoing->at(2).flatten();
        const auto fourth = outgoing->at(3).flatten();
        const auto fifth = outgoing->at(4).flatten();

        ok &= check(frameType(first) == static_cast<uint8_t>(Http2FrameType::Headers) &&
                        frameType(second) == static_cast<uint8_t>(Http2FrameType::Continuation) &&
                        frameType(third) == static_cast<uint8_t>(Http2FrameType::Continuation),
                    "combined path must split oversized header block into HEADERS plus CONTINUATION");
        ok &= check(frameType(fourth) == static_cast<uint8_t>(Http2FrameType::Data) &&
                        frameType(fifth) == static_cast<uint8_t>(Http2FrameType::Data),
                    "combined path must keep trailing payload frames as DATA");
        ok &= check(framePayloadLength(first) == kMinFrameSize &&
                        framePayloadLength(second) == kMinFrameSize &&
                        framePayloadLength(third) == 7 &&
                        framePayloadLength(fourth) == kMinFrameSize &&
                        framePayloadLength(fifth) == 1,
                    "combined path must apply peer MAX_FRAME_SIZE to header and data fragments");
        ok &= check(frameStreamId(first) == 7 &&
                        frameStreamId(second) == 7 &&
                        frameStreamId(third) == 7 &&
                        frameStreamId(fourth) == 7 &&
                        frameStreamId(fifth) == 7,
                    "combined path fragments must stay on the same stream");
        ok &= check((frameFlags(first) & Http2FrameFlags::kEndHeaders) == 0 &&
                        (frameFlags(second) & Http2FrameFlags::kEndHeaders) == 0 &&
                        (frameFlags(third) & Http2FrameFlags::kEndHeaders) != 0,
                    "combined path must only mark END_HEADERS on the final header fragment");
        ok &= check((frameFlags(first) & Http2FrameFlags::kEndStream) == 0 &&
                        (frameFlags(second) & Http2FrameFlags::kEndStream) == 0 &&
                        (frameFlags(third) & Http2FrameFlags::kEndStream) == 0 &&
                        (frameFlags(fourth) & Http2FrameFlags::kEndStream) == 0 &&
                        (frameFlags(fifth) & Http2FrameFlags::kEndStream) != 0,
                    "combined path must only mark END_STREAM on the final data fragment");
    }

    ok &= check(stream->state() == Http2StreamState::HalfClosedLocal,
                "combined path with terminal DATA must advance stream state after successful send");
    ok &= check(stream->isEndStreamSent(),
                "combined path with terminal DATA must mark END_STREAM sent");
    return ok;
}

bool assertOutboundCombinedHeaderBlockCapRejectsOversize() {
    bool ok = true;
    galay::async::TcpSocket socket(GHandle{-1});
    Http2Conn conn(std::move(socket));
    Http2StreamManager manager(conn);

    ok &= check(conn.applyPeerSettings(
        settingsFrame(Http2SettingsId::MaxHeaderListSize, 64)) == Http2ErrorCode::NoError,
        "peer MAX_HEADER_LIST_SIZE setup for outbound combined cap must succeed");

    auto stream = conn.createStream(9);
    manager.attachStreamIO(stream);

    stream->sendEncodedHeadersAndData(std::string(65, 'c'), std::string("x"), true);

    ok &= check(!manager.m_send_channel.tryRecv().has_value(),
                "combined outbound header block above peer MAX_HEADER_LIST_SIZE must not be enqueued");
    ok &= check(stream->state() == Http2StreamState::Idle,
                "rejected combined outbound header block must not advance stream state");
    ok &= check(!stream->isEndStreamSent(),
                "rejected combined outbound header block must not mark END_STREAM sent");
    return ok;
}

bool assertOutboundDataBatchSplitsByPeerMaxFrameSize() {
    bool ok = true;
    galay::async::TcpSocket socket(GHandle{-1});
    Http2Conn conn(std::move(socket));

    ok &= check(conn.applyPeerSettings(
        settingsFrame(Http2SettingsId::MaxFrameSize, kMinFrameSize)) == Http2ErrorCode::NoError,
        "peer MAX_FRAME_SIZE setup for outbound DATA batch split must succeed");

    std::vector<Http2OutgoingFrame> send_queue;
    auto stream = conn.createStream(11);
    stream->attachIO(&send_queue, nullptr, nullptr, nullptr, conn.peerSettings().max_frame_size);

    std::vector<std::string> chunks = {std::string(kMinFrameSize + 1, 'b')};
    stream->sendDataBatch(chunks, true);

    ok &= check(send_queue.size() == 2,
                "outbound DATA batch larger than peer MAX_FRAME_SIZE must split into fragments");
    if (send_queue.size() == 2) {
        const auto first = send_queue[0].flatten();
        const auto second = send_queue[1].flatten();

        ok &= check(frameType(first) == static_cast<uint8_t>(Http2FrameType::Data) &&
                        frameType(second) == static_cast<uint8_t>(Http2FrameType::Data),
                    "outbound DATA batch split fragments must remain DATA frames");
        ok &= check(framePayloadLength(first) == kMinFrameSize &&
                        framePayloadLength(second) == 1,
                    "outbound DATA batch split fragments must obey peer MAX_FRAME_SIZE");
        ok &= check((frameFlags(first) & Http2FrameFlags::kEndStream) == 0 &&
                        (frameFlags(second) & Http2FrameFlags::kEndStream) != 0,
                    "outbound DATA batch split must mark END_STREAM only on the final fragment");
    }
    return ok;
}

bool assertOutboundDataChunksSplitByPeerMaxFrameSize() {
    bool ok = true;
    galay::async::TcpSocket socket(GHandle{-1});
    Http2Conn conn(std::move(socket));

    ok &= check(conn.applyPeerSettings(
        settingsFrame(Http2SettingsId::MaxFrameSize, kMinFrameSize)) == Http2ErrorCode::NoError,
        "peer MAX_FRAME_SIZE setup for outbound DATA chunks split must succeed");

    std::vector<Http2OutgoingFrame> send_queue;
    auto stream = conn.createStream(13);
    stream->attachIO(&send_queue, nullptr, nullptr, nullptr, conn.peerSettings().max_frame_size);

    std::vector<std::string> chunks;
    chunks.emplace_back(kMinFrameSize + 1, 'u');
    chunks.emplace_back("v");
    stream->sendDataChunks(std::move(chunks), true);

    ok &= check(send_queue.size() == 3,
                "outbound DATA chunks path must split oversized chunks by peer MAX_FRAME_SIZE");
    if (send_queue.size() == 3) {
        const auto first = send_queue[0].flatten();
        const auto second = send_queue[1].flatten();
        const auto third = send_queue[2].flatten();

        ok &= check(frameType(first) == static_cast<uint8_t>(Http2FrameType::Data) &&
                        frameType(second) == static_cast<uint8_t>(Http2FrameType::Data) &&
                        frameType(third) == static_cast<uint8_t>(Http2FrameType::Data),
                    "outbound DATA chunks split fragments must remain DATA frames");
        ok &= check(framePayloadLength(first) == kMinFrameSize &&
                        framePayloadLength(second) == 1 &&
                        framePayloadLength(third) == 1,
                    "outbound DATA chunks split fragments must obey peer MAX_FRAME_SIZE");
        ok &= check((frameFlags(first) & Http2FrameFlags::kEndStream) == 0 &&
                        (frameFlags(second) & Http2FrameFlags::kEndStream) == 0 &&
                        (frameFlags(third) & Http2FrameFlags::kEndStream) != 0,
                    "outbound DATA chunks split must mark END_STREAM only on the final chunk fragment");
    }
    return ok;
}

} // namespace

int main() {
    bool ok = true;
    ok &= assertInboundUsesLocalMaxFrameSizeForRejection();
    ok &= assertInboundIgnoresPeerMaxFrameSizeForAcceptance();
    ok &= assertOutboundDataSplitsByPeerMaxFrameSize();
    ok &= assertOutboundHeadersSplitByPeerMaxFrameSize();
    ok &= assertOutboundHeaderBlockCapRejectsOversize();
    ok &= assertOutboundCombinedHeadersAndDataRespectPeerLimits();
    ok &= assertOutboundCombinedHeaderBlockCapRejectsOversize();
    ok &= assertOutboundDataBatchSplitsByPeerMaxFrameSize();
    ok &= assertOutboundDataChunksSplitByPeerMaxFrameSize();
    if (!ok) {
        return 1;
    }

    std::cout << "T86-H2LimitsProd PASS\n";
    return 0;
}
