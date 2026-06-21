#include "result_writer.h"

#include <galay/cpp/galay-rpc/kernel/rpc_conn.h>
#include <galay/cpp/galay-rpc/kernel/rpc_stream.h>

#include <array>
#include <cstring>
#include <iostream>
#include <span>
#include <string>
#include <vector>

using namespace galay::rpc;
using namespace galay::rpc::detail;

namespace {

std::span<const iovec> asSpan(const std::array<iovec, 2>& iovecs, size_t count)
{
    return std::span<const iovec>(iovecs.data(), count);
}

std::vector<iovec> splitBytes(const std::vector<char>& bytes, size_t split)
{
    return {
        iovec{const_cast<char*>(bytes.data()), split},
        iovec{const_cast<char*>(bytes.data() + split), bytes.size() - split}
    };
}

void writeAll(RingBuffer& ring, const std::vector<char>& bytes)
{
    const size_t written = ring.write(bytes.data(), bytes.size());
    if (written != bytes.size()) {
        throw std::runtime_error("ring write truncated");
    }
}

void test_iovec_split_header_request_parse(test::TestResultWriter& writer)
{
    RpcRequest request(1001, "BoundaryService", "SplitHeader");
    const std::string payload = "payload";
    request.payload(payload.data(), payload.size());
    const auto encoded = request.serialize();

    auto iovecs = splitBytes(encoded, 7);
    RpcRequest parsed;
    auto result = tryParseRequestMessage(std::span<const iovec>(iovecs.data(), iovecs.size()),
                                         encoded.size(),
                                         RPC_MAX_BODY_SIZE,
                                         parsed);

    writer.writeTestCase("iovec split header request parse",
        result.has_value() &&
        result.value() == encoded.size() &&
        parsed.requestId() == 1001 &&
        parsed.serviceName() == "BoundaryService" &&
        parsed.methodName() == "SplitHeader" &&
        std::string(parsed.payload().data(), parsed.payload().size()) == payload);
}

void test_iovec_incomplete_body_returns_zero(test::TestResultWriter& writer)
{
    RpcRequest request(1002, "BoundaryService", "Incomplete");
    const std::string payload = "body";
    request.payload(payload.data(), payload.size());
    const auto encoded = request.serialize();

    auto iovecs = splitBytes(encoded, RPC_HEADER_SIZE);
    RpcRequest parsed;
    auto result = tryParseRequestMessage(std::span<const iovec>(iovecs.data(), iovecs.size()),
                                         encoded.size() - 1,
                                         RPC_MAX_BODY_SIZE,
                                         parsed);

    writer.writeTestCase("iovec incomplete body returns 0",
        result.has_value() && result.value() == 0);
}

void test_iovec_response_body_short_rejected(test::TestResultWriter& writer)
{
    std::vector<char> encoded(RPC_HEADER_SIZE + 1, 0);
    RpcHeader header;
    header.m_type = static_cast<uint8_t>(RpcMessageType::RESPONSE);
    header.m_request_id = 1003;
    header.m_body_length = 1;
    header.serialize(encoded.data());

    auto iovecs = splitBytes(encoded, RPC_HEADER_SIZE);
    RpcResponse parsed;
    auto result = tryParseResponseMessage(std::span<const iovec>(iovecs.data(), iovecs.size()),
                                          encoded.size(),
                                          RPC_MAX_BODY_SIZE,
                                          parsed);

    writer.writeTestCase("iovec response body short rejected",
        !result.has_value() &&
        result.error().code() == RpcErrorCode::DESERIALIZATION_ERROR);
}

void test_iovec_payload_requires_at_most_two_segments(test::TestResultWriter& writer)
{
    RpcRequest request(1004, "S", "M");
    const std::string payload = "abc";
    request.payload(payload.data(), payload.size());
    const auto encoded = request.serialize();

    const size_t payload_offset = encoded.size() - payload.size();
    std::array<iovec, 4> iovecs{
        iovec{const_cast<char*>(encoded.data()), payload_offset},
        iovec{const_cast<char*>(encoded.data() + payload_offset), 1},
        iovec{const_cast<char*>(encoded.data() + payload_offset + 1), 1},
        iovec{const_cast<char*>(encoded.data() + payload_offset + 2), 1}
    };

    RpcRequest parsed;
    auto result = tryParseRequestMessage(std::span<const iovec>(iovecs.data(), iovecs.size()),
                                         encoded.size(),
                                         RPC_MAX_BODY_SIZE,
                                         parsed);

    writer.writeTestCase("iovec payload over two segments rejected",
        !result.has_value() &&
        result.error().code() == RpcErrorCode::DESERIALIZATION_ERROR);
}

void test_ring_buffer_wrapped_request_parse_consumes_frame(test::TestResultWriter& writer)
{
    RpcRequest request(1005, "WrapService", "Echo");
    const std::string payload = "wrap-payload";
    request.payload(payload.data(), payload.size());
    const auto encoded = request.serialize();

    RingBuffer ring(encoded.size() + 4);
    const std::string padding = "xxxxx";
    ring.write(padding.data(), padding.size());
    ring.consume(padding.size() - 1);
    writeAll(ring, encoded);

    std::array<iovec, 2> iovecs{};
    const size_t count = ring.getReadIovecs(iovecs);
    iovecs[0].iov_base = static_cast<char*>(iovecs[0].iov_base) + 1;
    iovecs[0].iov_len -= 1;
    RpcRequest parsed;
    auto result = tryParseRequestMessage(asSpan(iovecs, count),
                                         iovecsReadableBytes(asSpan(iovecs, count)),
                                         RPC_MAX_BODY_SIZE,
                                         parsed);
    if (result.has_value() && result.value() > 0) {
        ring.consume(result.value() + 1);
    }

    writer.writeTestCase("ring buffer wrapped request parse consumes frame",
        count == 2 &&
        result.has_value() &&
        result.value() == encoded.size() &&
        ring.readable() == 0 &&
        parsed.serviceName() == "WrapService" &&
        parsed.methodName() == "Echo" &&
        std::string(parsed.payload().data(), parsed.payload().size()) == payload);
}

void test_stream_message_ring_boundary(test::TestResultWriter& writer)
{
    StreamMessage message;
    message.streamId(1006);
    message.payload("stream-boundary");
    const auto encoded = message.serialize(RpcMessageType::STREAM_DATA);

    RingBuffer ring(encoded.size() + 3);
    const std::string padding = "abc";
    ring.write(padding.data(), padding.size());
    ring.consume(padding.size());
    writeAll(ring, encoded);

    StreamMessage parsed;
    StreamMessageReadState state(ring, parsed);
    const bool parsed_now = state.parseFromRingBuffer();

    writer.writeTestCase("stream message ring boundary parse",
        parsed_now &&
        ring.readable() == 0 &&
        parsed.streamId() == 1006 &&
        parsed.messageType() == RpcMessageType::STREAM_DATA &&
        parsed.payloadEquals("stream-boundary"));
}

} // namespace

int main()
{
    test::TestResultWriter writer("t5_reader_writer_boundary.result");
    std::cout << "Running RPC reader/writer boundary tests...\n";

    test_iovec_split_header_request_parse(writer);
    test_iovec_incomplete_body_returns_zero(writer);
    test_iovec_response_body_short_rejected(writer);
    test_iovec_payload_requires_at_most_two_segments(writer);
    test_ring_buffer_wrapped_request_parse_consumes_frame(writer);
    test_stream_message_ring_boundary(writer);

    writer.writeSummary();
    std::cout << "Tests completed. Passed: " << writer.passed()
              << ", Failed: " << writer.failed() << "\n";
    return writer.failed() > 0 ? 1 : 0;
}
