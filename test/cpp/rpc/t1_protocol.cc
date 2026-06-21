/**
 * @file t1_proto.cpp
 * @brief RPC协议测试
 */

#include "result_writer.h"
#include <galay/cpp/galay-rpc/kernel/rpc_stream.h>
#include <galay/cpp/galay-rpc/protoc/rpc_message.h>
#include <galay/cpp/galay-rpc/protoc/rpc_codec.h>
#include <iostream>
#include <cstring>

using namespace galay::rpc;

void test_rpc_header(test::TestResultWriter& writer) {
    RpcHeader header;
    header.m_type = static_cast<uint8_t>(RpcMessageType::REQUEST);
    header.m_request_id = 12345;
    header.m_body_length = 100;

    char buffer[RPC_HEADER_SIZE];
    header.serialize(buffer);

    RpcHeader parsed;
    bool success = parsed.deserialize(buffer);

    writer.writeTestCase("RpcHeader serialize/deserialize",
        success &&
        parsed.m_magic == RPC_MAGIC &&
        parsed.m_version == RPC_VERSION &&
        parsed.m_type == static_cast<uint8_t>(RpcMessageType::REQUEST) &&
        parsed.m_request_id == 12345 &&
        parsed.m_body_length == 100);
}

void test_rpc_request(test::TestResultWriter& writer) {
    RpcRequest request(1001, "TestService", "testMethod");
    std::string payload = "Hello, RPC!";
    request.payload(payload.data(), payload.size());

    auto serialized = request.serialize();

    auto result = RpcCodec::decodeRequest(serialized.data(), serialized.size());

    writer.writeTestCase("RpcRequest serialize/deserialize",
        result.has_value() &&
        result->requestId() == 1001 &&
        result->serviceName() == "TestService" &&
        result->methodName() == "testMethod" &&
        std::string(result->payload().data(), result->payload().size()) == payload);
}

void test_rpc_response(test::TestResultWriter& writer) {
    RpcResponse response(2001, RpcErrorCode::OK);
    std::string payload = "Response data";
    response.payload(payload.data(), payload.size());

    auto serialized = response.serialize();

    auto result = RpcCodec::decodeResponse(serialized.data(), serialized.size());

    writer.writeTestCase("RpcResponse serialize/deserialize",
        result.has_value() &&
        result->requestId() == 2001 &&
        result->errorCode() == RpcErrorCode::OK &&
        std::string(result->payload().data(), result->payload().size()) == payload);
}

void test_rpc_response_error(test::TestResultWriter& writer) {
    RpcResponse response(3001, RpcErrorCode::SERVICE_NOT_FOUND);

    auto serialized = response.serialize();
    auto result = RpcCodec::decodeResponse(serialized.data(), serialized.size());

    writer.writeTestCase("RpcResponse error code",
        result.has_value() &&
        result->requestId() == 3001 &&
        result->errorCode() == RpcErrorCode::SERVICE_NOT_FOUND &&
        !result->isOk());
}

void test_message_length(test::TestResultWriter& writer) {
    RpcRequest request(100, "Svc", "Method");
    auto serialized = request.serialize();

    size_t len = RpcCodec::messageLength(serialized.data(), serialized.size());

    writer.writeTestCase("RpcCodec messageLength",
        len == serialized.size());
}

void test_message_length_rejects_bad_header(test::TestResultWriter& writer) {
    char invalid_data[RPC_HEADER_SIZE] = {0};

    const size_t len = RpcCodec::messageLength(invalid_data, sizeof(invalid_data));

    writer.writeTestCase("RpcCodec messageLength rejects bad header", len == 0);
}

void test_invalid_header(test::TestResultWriter& writer) {
    char invalid_data[RPC_HEADER_SIZE] = {0};

    RpcHeader header;
    bool success = header.deserialize(invalid_data);

    writer.writeTestCase("Invalid header detection", !success);
}

void test_incomplete_data(test::TestResultWriter& writer) {
    char partial_data[8] = {0};

    RpcHeader header;
    auto result = RpcCodec::decodeHeader(partial_data, 8, header);

    writer.writeTestCase("Incomplete data detection",
        result == DecodeResult::INCOMPLETE);
}

void test_decode_rejects_truncated_request_body(test::TestResultWriter& writer) {
    RpcRequest request(810, "BoundaryService", "truncated");
    const std::string payload = "payload";
    request.payload(payload.data(), payload.size());

    auto serialized = request.serialize();
    serialized.pop_back();

    auto result = RpcCodec::decodeRequest(serialized.data(), serialized.size());

    writer.writeTestCase("RpcCodec decodeRequest rejects truncated body",
        !result.has_value());
}

void test_decode_rejects_too_short_response_body(test::TestResultWriter& writer) {
    std::vector<char> serialized(RPC_HEADER_SIZE + 1, '\0');

    RpcHeader header;
    header.m_type = static_cast<uint8_t>(RpcMessageType::RESPONSE);
    header.m_request_id = 811;
    header.m_body_length = 1;
    header.serialize(serialized.data());
    serialized[RPC_HEADER_SIZE] = 0;

    auto result = RpcCodec::decodeResponse(serialized.data(), serialized.size());

    writer.writeTestCase("RpcCodec decodeResponse rejects too short body",
        !result.has_value());
}

void test_decode_rejects_oversized_header_body(test::TestResultWriter& writer) {
    char serialized[RPC_HEADER_SIZE] = {0};

    RpcHeader header;
    header.m_type = static_cast<uint8_t>(RpcMessageType::REQUEST);
    header.m_request_id = 812;
    header.m_body_length = RPC_MAX_BODY_SIZE + 1;
    header.serialize(serialized);

    auto result = RpcCodec::decodeRequest(serialized, sizeof(serialized));

    writer.writeTestCase("RpcCodec decodeRequest rejects oversized body",
        !result.has_value());
}

void test_empty_payload(test::TestResultWriter& writer) {
    RpcRequest request(500, "EmptyService", "emptyMethod");
    auto serialized = request.serialize();

    auto result = RpcCodec::decodeRequest(serialized.data(), serialized.size());

    writer.writeTestCase("Empty payload request",
        result.has_value() &&
        result->payload().empty());
}

void test_large_payload(test::TestResultWriter& writer) {
    RpcRequest request(600, "LargeService", "largeMethod");
    std::vector<char> large_payload(1024 * 1024, 'X');  // 1MB
    request.payload(large_payload.data(), large_payload.size());

    auto serialized = request.serialize();
    auto result = RpcCodec::decodeRequest(serialized.data(), serialized.size());

    writer.writeTestCase("Large payload (1MB)",
        result.has_value() &&
        result->payload().size() == large_payload.size());
}

void test_rpc_call_mode_flags(test::TestResultWriter& writer) {
    const std::string payload = "stream-frame";

    RpcRequest request(700, "StreamService", "upload");
    request.callMode(RpcCallMode::CLIENT_STREAMING);
    request.endOfStream(false);
    request.payload(payload.data(), payload.size());

    auto request_serialized = request.serialize();
    RpcHeader req_header;
    const bool req_header_ok = req_header.deserialize(request_serialized.data());
    auto request_decoded = RpcCodec::decodeRequest(request_serialized.data(), request_serialized.size());

    writer.writeTestCase("RpcRequest call mode flags",
        req_header_ok &&
        rpcDecodeCallMode(req_header.m_flags) == RpcCallMode::CLIENT_STREAMING &&
        !rpcIsEndStream(req_header.m_flags) &&
        request_decoded.has_value() &&
        request_decoded->callMode() == RpcCallMode::CLIENT_STREAMING &&
        !request_decoded->endOfStream() &&
        std::string(request_decoded->payload().data(), request_decoded->payload().size()) == payload);

    RpcResponse response(700, RpcErrorCode::OK);
    response.callMode(RpcCallMode::SERVER_STREAMING);
    response.endOfStream(false);
    response.payload(payload.data(), payload.size());

    auto response_serialized = response.serialize();
    RpcHeader resp_header;
    const bool resp_header_ok = resp_header.deserialize(response_serialized.data());
    auto response_decoded = RpcCodec::decodeResponse(response_serialized.data(), response_serialized.size());

    writer.writeTestCase("RpcResponse call mode flags",
        resp_header_ok &&
        rpcDecodeCallMode(resp_header.m_flags) == RpcCallMode::SERVER_STREAMING &&
        !rpcIsEndStream(resp_header.m_flags) &&
        response_decoded.has_value() &&
        response_decoded->callMode() == RpcCallMode::SERVER_STREAMING &&
        !response_decoded->endOfStream() &&
        std::string(response_decoded->payload().data(), response_decoded->payload().size()) == payload);
}

void test_stream_message_borrowed_payload(test::TestResultWriter& writer) {
    static const char segment1[] = "hello";
    static const char segment2[] = "world";

    StreamMessage message;
    message.streamId(900);
    message.payloadView(RpcPayloadView{
        segment1,
        sizeof(segment1) - 1,
        segment2,
        sizeof(segment2) - 1
    });

    const auto view = message.payloadView();
    writer.writeTestCase("StreamMessage borrowed payload view",
        message.streamId() == 900 &&
        message.payloadSize() == 10 &&
        view.segment1_len == 5 &&
        view.segment2_len == 5 &&
        std::string(message.payload().data(), message.payload().size()) == "helloworld");
}

void test_stream_message_borrowed_payload_serialize(test::TestResultWriter& writer) {
    static const char segment1[] = "hello";
    static const char segment2[] = "world";

    StreamMessage message;
    message.streamId(902);
    message.payloadView(RpcPayloadView{
        segment1,
        sizeof(segment1) - 1,
        segment2,
        sizeof(segment2) - 1
    });

    const auto serialized = message.serialize(RpcMessageType::STREAM_DATA);
    RpcHeader header;
    const bool header_ok = header.deserialize(serialized.data());
    const std::string body(serialized.data() + RPC_HEADER_SIZE,
                           serialized.data() + serialized.size());
    const bool passed =
        header_ok &&
        header.m_type == static_cast<uint8_t>(RpcMessageType::STREAM_DATA) &&
        header.m_request_id == 902 &&
        header.m_body_length == 10 &&
        body == "helloworld";

    writer.writeTestCase("StreamMessage borrowed payload serialize",
        passed,
        passed ? "" :
            ("type=" + std::to_string(static_cast<int>(header.m_type)) +
             ", request_id=" + std::to_string(header.m_request_id) +
             ", body_length=" + std::to_string(header.m_body_length) +
             ", body=" + body));
}

void test_rpc_request_borrowed_payload_serialize(test::TestResultWriter& writer) {
    static const char segment1[] = "hello";
    static const char segment2[] = "rpc";

    RpcRequest request(820, "BorrowedService", "request");
    request.payloadView(RpcPayloadView{
        segment1,
        sizeof(segment1) - 1,
        segment2,
        sizeof(segment2) - 1
    });

    const auto serialized = request.serialize();
    auto decoded = RpcCodec::decodeRequest(serialized.data(), serialized.size());
    const bool passed =
        decoded.has_value() &&
        decoded->requestId() == 820 &&
        decoded->serviceName() == "BorrowedService" &&
        decoded->methodName() == "request" &&
        std::string(decoded->payload().data(), decoded->payload().size()) == "hellorpc";

    writer.writeTestCase("RpcRequest borrowed payload serialize", passed);
}

void test_rpc_request_metadata_round_trip(test::TestResultWriter& writer) {
    RpcRequest request(830, "MetaService", "echo");
    const std::string payload = "metadata-payload";
    request.payload(payload.data(), payload.size());
    auto inserted_auth = request.metadata().insert("authorization", "token");
    auto inserted_trace = request.metadata().insert("traceparent", "00-abc-def-01");

    const auto serialized = request.serialize();
    auto decoded = RpcCodec::decodeRequest(serialized.data(), serialized.size());
    auto auth = decoded.has_value() ? decoded->metadata().get("authorization") : std::nullopt;
    auto trace = decoded.has_value() ? decoded->metadata().get("traceparent") : std::nullopt;
    const bool passed =
        inserted_auth.has_value() &&
        inserted_trace.has_value() &&
        decoded.has_value() &&
        decoded->requestId() == 830 &&
        decoded->serviceName() == "MetaService" &&
        decoded->methodName() == "echo" &&
        auth.has_value() && *auth == "token" &&
        trace.has_value() && *trace == "00-abc-def-01" &&
        std::string(decoded->payload().data(), decoded->payload().size()) == payload;

    writer.writeTestCase("RpcRequest metadata round trip", passed);
}

void test_rpc_request_rejects_truncated_metadata(test::TestResultWriter& writer) {
    RpcRequest request(831, "MetaService", "echo");
    auto inserted = request.metadata().insert("authorization", "token");
    auto serialized = request.serialize();
    serialized.resize(serialized.size() - 5);

    auto decoded = RpcCodec::decodeRequest(serialized.data(), serialized.size());
    writer.writeTestCase("RpcRequest rejects truncated metadata",
        inserted.has_value() && !decoded.has_value());
}

void test_rpc_response_borrowed_payload_serialize(test::TestResultWriter& writer) {
    static const char segment1[] = "hello";
    static const char segment2[] = "rpc";

    RpcResponse response(821, RpcErrorCode::OK);
    response.payloadView(RpcPayloadView{
        segment1,
        sizeof(segment1) - 1,
        segment2,
        sizeof(segment2) - 1
    });

    const auto serialized = response.serialize();
    auto decoded = RpcCodec::decodeResponse(serialized.data(), serialized.size());
    const bool passed =
        decoded.has_value() &&
        decoded->requestId() == 821 &&
        decoded->errorCode() == RpcErrorCode::OK &&
        std::string(decoded->payload().data(), decoded->payload().size()) == "hellorpc";

    writer.writeTestCase("RpcResponse borrowed payload serialize", passed);
}

void test_stream_message_ctor_owns_payload(test::TestResultWriter& writer) {
    const std::string payload = "ctor-payload";
    StreamMessage message(901, payload.data(), payload.size());

    writer.writeTestCase("StreamMessage constructor stores payload",
        message.streamId() == 901 &&
        message.payloadSize() == payload.size() &&
        message.payloadStr() == payload);
}

int main() {
    test::TestResultWriter writer("t1_proto.result");

    std::cout << "Running RPC Protocol Tests...\n";

    test_rpc_header(writer);
    test_rpc_request(writer);
    test_rpc_response(writer);
    test_rpc_response_error(writer);
    test_message_length(writer);
    test_message_length_rejects_bad_header(writer);
    test_invalid_header(writer);
    test_incomplete_data(writer);
    test_decode_rejects_truncated_request_body(writer);
    test_decode_rejects_too_short_response_body(writer);
    test_decode_rejects_oversized_header_body(writer);
    test_empty_payload(writer);
    test_large_payload(writer);
    test_rpc_call_mode_flags(writer);
    test_stream_message_borrowed_payload(writer);
    test_stream_message_borrowed_payload_serialize(writer);
    test_rpc_request_borrowed_payload_serialize(writer);
    test_rpc_request_metadata_round_trip(writer);
    test_rpc_request_rejects_truncated_metadata(writer);
    test_rpc_response_borrowed_payload_serialize(writer);
    test_stream_message_ctor_owns_payload(writer);

    writer.writeSummary();

    std::cout << "Tests completed. Passed: " << writer.passed()
              << ", Failed: " << writer.failed() << "\n";

    return writer.failed() > 0 ? 1 : 0;
}
