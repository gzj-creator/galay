/**
 * @file T46-H2OutboundSegments.cc
 * @brief HTTP/2 outbound segmented packet contract
 */

#include <array>
#include <cassert>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>
#include <sys/uio.h>

#define private public
#include <galay/cpp/galay-http2/kernel/http2_stream.h>
#undef private
#include <galay/cpp/galay-http2/protoc/http2_frame.h>

using namespace galay::http2;

static std::string flattenIovecs(const std::array<struct iovec, 2>& iovecs, size_t count) {
    std::string out;
    for (size_t i = 0; i < count; ++i) {
        out.append(static_cast<const char*>(iovecs[i].iov_base), iovecs[i].iov_len);
    }
    return out;
}

int main() {
    static_assert(requires(std::array<char, kHttp2FrameHeaderLength> header) {
        { Http2OutgoingFrame::segmented(header, std::string("abc")) } -> std::same_as<Http2OutgoingFrame>;
    }, "Http2OutgoingFrame must expose segmented(header, owned_payload)");

    static_assert(requires(std::array<char, kHttp2FrameHeaderLength> header,
                           std::shared_ptr<const std::string> payload) {
        { Http2OutgoingFrame::segmentedShared(header, std::move(payload)) } -> std::same_as<Http2OutgoingFrame>;
    }, "Http2OutgoingFrame must expose segmentedShared(header, shared_payload)");

    static_assert(requires(const Http2OutgoingFrame& frame, std::array<struct iovec, 2>& iovecs) {
        { frame.isEmpty() } -> std::same_as<bool>;
        { frame.isSegmented() } -> std::same_as<bool>;
        { frame.flatten() } -> std::same_as<std::string>;
        { frame.exportIovecs(iovecs) } -> std::same_as<size_t>;
    }, "Http2OutgoingFrame must expose segmented packet inspection helpers");

    static_assert(requires(Http2Stream::ptr stream, const std::vector<Http2HeaderField>& headers, std::string body) {
        { stream->sendHeadersAndData(headers, std::move(body), true) } -> std::same_as<void>;
    }, "Http2Stream must expose batched headers+data send helper");

    static_assert(requires(Http2Stream::ptr stream, std::string header_block, std::string body) {
        { stream->sendEncodedHeadersAndData(std::move(header_block), std::move(body), true) } -> std::same_as<void>;
    }, "Http2Stream must expose batched encoded-headers+data send helper");

    static_assert(requires(Http2Stream::ptr stream,
                           std::shared_ptr<const std::string> header_block,
                           std::string body) {
        { stream->sendEncodedHeadersAndData(std::move(header_block), std::move(body), true) } -> std::same_as<void>;
    }, "Http2Stream must expose shared encoded-headers+data send helper");

    Http2OutgoingFrame empty;
    assert(empty.isEmpty());
    assert(!empty.isSegmented());

    const std::string payload = "payload";
    const auto header = Http2FrameBuilder::dataHeaderBytes(9, payload.size(), true);
    auto segmented = Http2OutgoingFrame::segmented(header, std::string(payload));

    assert(!segmented.isEmpty());
    assert(segmented.isSegmented());
    assert(segmented.flatten() == Http2FrameBuilder::dataBytes(9, payload, true));

    std::array<struct iovec, 2> segmented_iovecs{};
    const size_t segmented_count = segmented.exportIovecs(segmented_iovecs);
    assert(segmented_count == 2);
    assert(flattenIovecs(segmented_iovecs, segmented_count) ==
           Http2FrameBuilder::dataBytes(9, payload, true));

    auto shared_payload = std::make_shared<const std::string>("shared-payload");
    const auto shared_header = Http2FrameBuilder::dataHeaderBytes(9, shared_payload->size(), true);
    auto shared_segmented = Http2OutgoingFrame::segmentedShared(shared_header, shared_payload);
    assert(!shared_segmented.isEmpty());
    assert(shared_segmented.isSegmented());
    assert(shared_segmented.flatten() == Http2FrameBuilder::dataBytes(9, *shared_payload, true));

    std::array<struct iovec, 2> shared_iovecs{};
    const size_t shared_count = shared_segmented.exportIovecs(shared_iovecs);
    assert(shared_count == 2);
    assert(shared_iovecs[1].iov_base == const_cast<char*>(shared_payload->data()));
    assert(shared_iovecs[1].iov_len == shared_payload->size());
    assert(flattenIovecs(shared_iovecs, shared_count) ==
           Http2FrameBuilder::dataBytes(9, *shared_payload, true));

    Http2OutgoingFrame serialized(std::string("serialized"));
    assert(!serialized.isEmpty());
    assert(!serialized.isSegmented());
    assert(serialized.flatten() == "serialized");

    std::array<struct iovec, 2> serialized_iovecs{};
    const size_t serialized_count = serialized.exportIovecs(serialized_iovecs);
    assert(serialized_count == 1);
    assert(flattenIovecs(serialized_iovecs, serialized_count) == "serialized");

    std::vector<Http2OutgoingFrame> send_queue;
    auto stream = Http2Stream::create(11);
    stream->attachIO(&send_queue, nullptr, nullptr);

    HpackEncoder encoder;
    const std::string encoded_headers = encoder.encode({
        {":status", "200"},
        {"content-length", "3"},
    });
    auto shared_headers = std::make_shared<const std::string>(encoded_headers);
    auto shared_data = std::make_shared<const std::string>("xyz");

    stream->sendEncodedHeaders(std::string(encoded_headers), true, true);

    assert(send_queue.size() == 1);
    assert(stream->isEndStreamSent());
    assert(stream->state() == Http2StreamState::HalfClosedLocal);
    assert(send_queue.back().isSegmented());
    assert(send_queue.back().flatten() ==
           Http2FrameBuilder::headersBytes(11, encoded_headers, true, true));

    std::vector<Http2OutgoingFrame> shared_send_queue;
    auto shared_header_stream = Http2Stream::create(13);
    shared_header_stream->attachIO(&shared_send_queue, nullptr, nullptr);
    shared_header_stream->sendEncodedHeaders(shared_headers, true, true);
    assert(shared_send_queue.size() == 1);
    std::array<struct iovec, 2> shared_header_frame_iovecs{};
    const size_t shared_header_frame_count =
        shared_send_queue.back().exportIovecs(shared_header_frame_iovecs);
    assert(shared_header_frame_count == 2);
    assert(shared_header_frame_iovecs[1].iov_base == const_cast<char*>(shared_headers->data()));
    assert(shared_send_queue.back().flatten() ==
           Http2FrameBuilder::headersBytes(13, *shared_headers, true, true));

    std::vector<Http2OutgoingFrame> shared_data_queue;
    auto shared_data_stream = Http2Stream::create(15);
    shared_data_stream->attachIO(&shared_data_queue, nullptr, nullptr);
    shared_data_stream->sendData(shared_data, true);
    assert(shared_data_queue.size() == 1);
    std::array<struct iovec, 2> shared_data_frame_iovecs{};
    const size_t shared_data_frame_count =
        shared_data_queue.back().exportIovecs(shared_data_frame_iovecs);
    assert(shared_data_frame_count == 2);
    assert(shared_data_frame_iovecs[1].iov_base == const_cast<char*>(shared_data->data()));
    assert(shared_data_queue.back().flatten() ==
           Http2FrameBuilder::dataBytes(15, *shared_data, true));

    std::vector<Http2OutgoingFrame> chunk_queue;
    auto chunk_stream = Http2Stream::create(17);
    chunk_stream->attachIO(&chunk_queue, nullptr, nullptr);
    std::vector<std::string> chunks;
    chunks.push_back("he");
    chunks.push_back("llo");
    chunk_stream->sendDataChunks(std::move(chunks), true);
    assert(chunk_queue.size() == 2);
    assert(chunk_queue[0].isSegmented());
    assert(chunk_queue[1].isSegmented());
    assert(chunk_queue[0].flatten() == Http2FrameBuilder::dataBytes(17, "he", false));
    assert(chunk_queue[1].flatten() == Http2FrameBuilder::dataBytes(17, "llo", true));

    std::vector<Http2OutgoingFrame> combined_queue;
    auto combined_stream = Http2Stream::create(19);
    HpackEncoder combined_encoder;
    combined_stream->attachIO(&combined_queue, &combined_encoder, nullptr);
    combined_stream->sendHeadersAndData({
        {":status", "200"},
        {"content-length", "3"},
    }, std::string("hey"), true);
    assert(combined_queue.size() == 2);
    assert(combined_queue[0].isSegmented());
    assert(combined_queue[1].isSegmented());
    HpackEncoder expected_encoder;
    const std::string combined_header_block = expected_encoder.encode({
        {":status", "200"},
        {"content-length", "3"},
    });
    assert(combined_queue[0].flatten() ==
           Http2FrameBuilder::headersBytes(19, combined_header_block, false, true));
    assert(combined_queue[1].flatten() == Http2FrameBuilder::dataBytes(19, "hey", true));

    std::vector<Http2OutgoingFrame> encoded_combined_queue;
    auto encoded_combined_stream = Http2Stream::create(21);
    encoded_combined_stream->attachIO(&encoded_combined_queue, nullptr, nullptr);
    encoded_combined_stream->sendEncodedHeadersAndData(std::string(encoded_headers), std::string("xyz"), true);
    assert(encoded_combined_queue.size() == 2);
    assert(encoded_combined_queue[0].flatten() ==
           Http2FrameBuilder::headersBytes(21, encoded_headers, false, true));
    assert(encoded_combined_queue[1].flatten() == Http2FrameBuilder::dataBytes(21, "xyz", true));

    std::vector<Http2OutgoingFrame> shared_encoded_combined_queue;
    auto shared_encoded_combined_stream = Http2Stream::create(23);
    shared_encoded_combined_stream->attachIO(&shared_encoded_combined_queue, nullptr, nullptr);
    shared_encoded_combined_stream->sendEncodedHeadersAndData(shared_headers, std::string("xyz"), true);
    assert(shared_encoded_combined_queue.size() == 2);
    std::array<struct iovec, 2> shared_encoded_header_iovecs{};
    const size_t shared_encoded_header_count =
        shared_encoded_combined_queue[0].exportIovecs(shared_encoded_header_iovecs);
    assert(shared_encoded_header_count == 2);
    assert(shared_encoded_header_iovecs[1].iov_base == const_cast<char*>(shared_headers->data()));
    assert(shared_encoded_combined_queue[0].flatten() ==
           Http2FrameBuilder::headersBytes(23, *shared_headers, false, true));
    assert(shared_encoded_combined_queue[1].flatten() == Http2FrameBuilder::dataBytes(23, "xyz", true));

    std::cout << "T46-H2OutboundSegments PASS\n";
    return 0;
}
