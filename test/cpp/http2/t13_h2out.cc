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

static std::string flatten_iovecs(const std::array<struct iovec, 2>& iovecs, size_t count) {
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
        { Http2OutgoingFrame::segmented_shared(header, std::move(payload)) } -> std::same_as<Http2OutgoingFrame>;
    }, "Http2OutgoingFrame must expose segmentedShared(header, shared_payload)");

    static_assert(requires(const Http2OutgoingFrame& frame, std::array<struct iovec, 2>& iovecs) {
        { frame.is_empty() } -> std::same_as<bool>;
        { frame.is_segmented() } -> std::same_as<bool>;
        { frame.flatten() } -> std::same_as<std::string>;
        { frame.export_iovecs(iovecs) } -> std::same_as<size_t>;
    }, "Http2OutgoingFrame must expose segmented packet inspection helpers");

    static_assert(requires(Http2Stream::ptr stream, const std::vector<Http2HeaderField>& headers, std::string body) {
        { stream->send_headers_and_data(headers, std::move(body), true) } -> std::same_as<void>;
    }, "Http2Stream must expose batched headers+data send helper");

    static_assert(requires(Http2Stream::ptr stream, std::string header_block, std::string body) {
        { stream->send_encoded_headers_and_data(std::move(header_block), std::move(body), true) } -> std::same_as<void>;
    }, "Http2Stream must expose batched encoded-headers+data send helper");

    static_assert(requires(Http2Stream::ptr stream,
                           std::shared_ptr<const std::string> header_block,
                           std::string body) {
        { stream->send_encoded_headers_and_data(std::move(header_block), std::move(body), true) } -> std::same_as<void>;
    }, "Http2Stream must expose shared encoded-headers+data send helper");

    Http2OutgoingFrame empty;
    assert(empty.is_empty());
    assert(!empty.is_segmented());

    const std::string payload = "payload";
    const auto header = Http2FrameBuilder::data_header_bytes(9, payload.size(), true);
    auto segmented = Http2OutgoingFrame::segmented(header, std::string(payload));

    assert(!segmented.is_empty());
    assert(segmented.is_segmented());
    assert(segmented.flatten() == Http2FrameBuilder::data_bytes(9, payload, true));

    std::array<struct iovec, 2> segmented_iovecs{};
    const size_t segmented_count = segmented.export_iovecs(segmented_iovecs);
    assert(segmented_count == 2);
    assert(flatten_iovecs(segmented_iovecs, segmented_count) ==
           Http2FrameBuilder::data_bytes(9, payload, true));

    auto shared_payload = std::make_shared<const std::string>("shared-payload");
    const auto shared_header = Http2FrameBuilder::data_header_bytes(9, shared_payload->size(), true);
    auto shared_segmented = Http2OutgoingFrame::segmented_shared(shared_header, shared_payload);
    assert(!shared_segmented.is_empty());
    assert(shared_segmented.is_segmented());
    assert(shared_segmented.flatten() == Http2FrameBuilder::data_bytes(9, *shared_payload, true));

    std::array<struct iovec, 2> shared_iovecs{};
    const size_t shared_count = shared_segmented.export_iovecs(shared_iovecs);
    assert(shared_count == 2);
    assert(shared_iovecs[1].iov_base == const_cast<char*>(shared_payload->data()));
    assert(shared_iovecs[1].iov_len == shared_payload->size());
    assert(flatten_iovecs(shared_iovecs, shared_count) ==
           Http2FrameBuilder::data_bytes(9, *shared_payload, true));

    Http2OutgoingFrame serialized(std::string("serialized"));
    assert(!serialized.is_empty());
    assert(!serialized.is_segmented());
    assert(serialized.flatten() == "serialized");

    std::array<struct iovec, 2> serialized_iovecs{};
    const size_t serialized_count = serialized.export_iovecs(serialized_iovecs);
    assert(serialized_count == 1);
    assert(flatten_iovecs(serialized_iovecs, serialized_count) == "serialized");

    std::vector<Http2OutgoingFrame> send_queue;
    auto stream = Http2Stream::create(11);
    stream->attach_io(&send_queue, nullptr, nullptr);

    HpackEncoder encoder;
    const std::string encoded_headers = encoder.encode({
        {":status", "200"},
        {"content-length", "3"},
    });
    auto shared_headers = std::make_shared<const std::string>(encoded_headers);
    auto shared_data = std::make_shared<const std::string>("xyz");

    stream->send_encoded_headers(std::string(encoded_headers), true, true);

    assert(send_queue.size() == 1);
    assert(stream->is_end_stream_sent());
    assert(stream->state() == Http2StreamState::HalfClosedLocal);
    assert(send_queue.back().is_segmented());
    assert(send_queue.back().flatten() ==
           Http2FrameBuilder::headers_bytes(11, encoded_headers, true, true));

    std::vector<Http2OutgoingFrame> shared_send_queue;
    auto shared_header_stream = Http2Stream::create(13);
    shared_header_stream->attach_io(&shared_send_queue, nullptr, nullptr);
    shared_header_stream->send_encoded_headers(shared_headers, true, true);
    assert(shared_send_queue.size() == 1);
    std::array<struct iovec, 2> shared_header_frame_iovecs{};
    const size_t shared_header_frame_count =
        shared_send_queue.back().export_iovecs(shared_header_frame_iovecs);
    assert(shared_header_frame_count == 2);
    assert(shared_header_frame_iovecs[1].iov_base == const_cast<char*>(shared_headers->data()));
    assert(shared_send_queue.back().flatten() ==
           Http2FrameBuilder::headers_bytes(13, *shared_headers, true, true));

    std::vector<Http2OutgoingFrame> shared_data_queue;
    auto shared_data_stream = Http2Stream::create(15);
    shared_data_stream->attach_io(&shared_data_queue, nullptr, nullptr);
    shared_data_stream->send_data(shared_data, true);
    assert(shared_data_queue.size() == 1);
    std::array<struct iovec, 2> shared_data_frame_iovecs{};
    const size_t shared_data_frame_count =
        shared_data_queue.back().export_iovecs(shared_data_frame_iovecs);
    assert(shared_data_frame_count == 2);
    assert(shared_data_frame_iovecs[1].iov_base == const_cast<char*>(shared_data->data()));
    assert(shared_data_queue.back().flatten() ==
           Http2FrameBuilder::data_bytes(15, *shared_data, true));

    std::vector<Http2OutgoingFrame> chunk_queue;
    auto chunk_stream = Http2Stream::create(17);
    chunk_stream->attach_io(&chunk_queue, nullptr, nullptr);
    std::vector<std::string> chunks;
    chunks.push_back("he");
    chunks.push_back("llo");
    chunk_stream->send_data_chunks(std::move(chunks), true);
    assert(chunk_queue.size() == 2);
    assert(chunk_queue[0].is_segmented());
    assert(chunk_queue[1].is_segmented());
    assert(chunk_queue[0].flatten() == Http2FrameBuilder::data_bytes(17, "he", false));
    assert(chunk_queue[1].flatten() == Http2FrameBuilder::data_bytes(17, "llo", true));

    std::vector<Http2OutgoingFrame> combined_queue;
    auto combined_stream = Http2Stream::create(19);
    HpackEncoder combined_encoder;
    combined_stream->attach_io(&combined_queue, &combined_encoder, nullptr);
    combined_stream->send_headers_and_data({
        {":status", "200"},
        {"content-length", "3"},
    }, std::string("hey"), true);
    assert(combined_queue.size() == 2);
    assert(combined_queue[0].is_segmented());
    assert(combined_queue[1].is_segmented());
    HpackEncoder expected_encoder;
    const std::string combined_header_block = expected_encoder.encode({
        {":status", "200"},
        {"content-length", "3"},
    });
    assert(combined_queue[0].flatten() ==
           Http2FrameBuilder::headers_bytes(19, combined_header_block, false, true));
    assert(combined_queue[1].flatten() == Http2FrameBuilder::data_bytes(19, "hey", true));

    std::vector<Http2OutgoingFrame> encoded_combined_queue;
    auto encoded_combined_stream = Http2Stream::create(21);
    encoded_combined_stream->attach_io(&encoded_combined_queue, nullptr, nullptr);
    encoded_combined_stream->send_encoded_headers_and_data(std::string(encoded_headers), std::string("xyz"), true);
    assert(encoded_combined_queue.size() == 2);
    assert(encoded_combined_queue[0].flatten() ==
           Http2FrameBuilder::headers_bytes(21, encoded_headers, false, true));
    assert(encoded_combined_queue[1].flatten() == Http2FrameBuilder::data_bytes(21, "xyz", true));

    std::vector<Http2OutgoingFrame> shared_encoded_combined_queue;
    auto shared_encoded_combined_stream = Http2Stream::create(23);
    shared_encoded_combined_stream->attach_io(&shared_encoded_combined_queue, nullptr, nullptr);
    shared_encoded_combined_stream->send_encoded_headers_and_data(shared_headers, std::string("xyz"), true);
    assert(shared_encoded_combined_queue.size() == 2);
    std::array<struct iovec, 2> shared_encoded_header_iovecs{};
    const size_t shared_encoded_header_count =
        shared_encoded_combined_queue[0].export_iovecs(shared_encoded_header_iovecs);
    assert(shared_encoded_header_count == 2);
    assert(shared_encoded_header_iovecs[1].iov_base == const_cast<char*>(shared_headers->data()));
    assert(shared_encoded_combined_queue[0].flatten() ==
           Http2FrameBuilder::headers_bytes(23, *shared_headers, false, true));
    assert(shared_encoded_combined_queue[1].flatten() == Http2FrameBuilder::data_bytes(23, "xyz", true));

    std::cout << "T46-H2OutboundSegments PASS\n";
    return 0;
}
