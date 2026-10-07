/**
 * @file t5_reader_writer_boundary.cc
 * @brief RPC reader/writer边界测试
 */

#include "result_writer.h"
#include <galay/cpp/galay-rpc/kernel/rpc_conn.h>
#include <galay/cpp/galay-rpc/protoc/rpc_codec.h>
#include <sys/uio.h>
#include <array>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

using namespace galay::rpc;

namespace {

std::vector<char> join_iovecs(const struct iovec* iovecs, size_t count) {
    std::vector<char> out;
    size_t total = 0;
    for (size_t i = 0; i < count; ++i) {
        total += iovecs[i].iov_len;
    }
    out.reserve(total);
    for (size_t i = 0; i < count; ++i) {
        const auto* data = static_cast<const char*>(iovecs[i].iov_base);
        out.insert(out.end(), data, data + iovecs[i].iov_len);
    }
    return out;
}

std::array<iovec, 2> split_message(std::vector<char>& message, size_t split_at) {
    return {
        iovec{message.data(), split_at},
        iovec{message.data() + split_at, message.size() - split_at}
    };
}

} // namespace

void test_request_payload_split_across_iovecs(test::TestResultWriter& writer) {
    RpcRequest request(1001, "BoundaryService", "splitRequest");
    const std::string first = "hello";
    const std::string second = "request";
    request.payload_view(RpcPayloadView{first.data(), first.size(), second.data(), second.size()});

    galay::rpc::detail::RpcRequestWriteState write_state(request);
    auto serialized = join_iovecs(write_state.write_iovecs_data(), write_state.write_iovecs_count());
    const size_t split_at = serialized.size() - second.size();
    auto iovecs = split_message(serialized, split_at);

    RpcRequest parsed;
    auto parsed_bytes = galay::rpc::detail::try_parse_request_message(iovecs, serialized.size(), RPC_MAX_BODY_SIZE, parsed);

    writer.write_test_case("RpcReader parses request payload split across iovecs",
        parsed_bytes.has_value() &&
        parsed_bytes.value() == serialized.size() &&
        parsed.request_id() == 1001 &&
        parsed.service_name() == "BoundaryService" &&
        parsed.method_name() == "splitRequest" &&
        std::string(parsed.payload().data(), parsed.payload().size()) == "hellorequest");
}

void test_request_metadata_split_across_iovecs(test::TestResultWriter& writer) {
    RpcRequest request(1006, "BoundaryService", "metadata");
    const std::string payload = "body";
    request.payload(payload.data(), payload.size());
    request.metadata().insert("authorization", "token");

    galay::rpc::detail::RpcRequestWriteState write_state(request);
    auto serialized = join_iovecs(write_state.write_iovecs_data(), write_state.write_iovecs_count());
    auto iovecs = split_message(serialized, RPC_HEADER_SIZE + 3);

    RpcRequest parsed;
    auto parsed_bytes = galay::rpc::detail::try_parse_request_message(iovecs, serialized.size(), RPC_MAX_BODY_SIZE, parsed);
    auto authorization = parsed.metadata().get("authorization");

    writer.write_test_case("RpcReader parses request metadata split across iovecs",
        parsed_bytes.has_value() &&
        parsed_bytes.value() == serialized.size() &&
        parsed.request_id() == 1006 &&
        parsed.service_name() == "BoundaryService" &&
        parsed.method_name() == "metadata" &&
        authorization.has_value() &&
        *authorization == "token" &&
        std::string(parsed.payload().data(), parsed.payload().size()) == payload);
}

void test_response_payload_split_across_iovecs(test::TestResultWriter& writer) {
    RpcResponse response(1002, RpcErrorCode::OK);
    const std::string first = "hello";
    const std::string second = "response";
    response.payload_view(RpcPayloadView{first.data(), first.size(), second.data(), second.size()});

    galay::rpc::detail::RpcResponseWriteState write_state(response);
    auto serialized = join_iovecs(write_state.write_iovecs_data(), write_state.write_iovecs_count());
    const size_t split_at = serialized.size() - second.size();
    auto iovecs = split_message(serialized, split_at);

    RpcResponse parsed;
    auto parsed_bytes = galay::rpc::detail::try_parse_response_message(iovecs, serialized.size(), RPC_MAX_BODY_SIZE, parsed);

    writer.write_test_case("RpcReader parses response payload split across iovecs",
        parsed_bytes.has_value() &&
        parsed_bytes.value() == serialized.size() &&
        parsed.request_id() == 1002 &&
        parsed.error_code() == RpcErrorCode::OK &&
        std::string(parsed.payload().data(), parsed.payload().size()) == "helloresponse");
}

void test_reader_setting_rejects_large_message(test::TestResultWriter& writer) {
    RpcRequest request(1003, "BoundaryService", "large");
    const std::string payload = "larger-than-limit";
    request.payload(payload.data(), payload.size());
    auto serialized = request.serialize();
    std::array<iovec, 1> iovecs{iovec{serialized.data(), serialized.size()}};

    RpcRequest parsed;
    auto parsed_bytes = galay::rpc::detail::try_parse_request_message(iovecs, serialized.size(), 4, parsed);

    writer.write_test_case("RpcReaderSetting max_message_size rejects larger message",
        !parsed_bytes.has_value() &&
        parsed_bytes.error().code() == RpcErrorCode::INVALID_REQUEST);
}

void test_wrong_message_type_rejected(test::TestResultWriter& writer) {
    RpcRequest request(1004, "BoundaryService", "wrongType");
    auto request_serialized = request.serialize();
    std::array<iovec, 1> request_iovecs{iovec{request_serialized.data(), request_serialized.size()}};

    RpcResponse response(1005, RpcErrorCode::OK);
    auto response_serialized = response.serialize();
    std::array<iovec, 1> response_iovecs{iovec{response_serialized.data(), response_serialized.size()}};

    RpcRequest parsed_request;
    auto request_result = galay::rpc::detail::try_parse_request_message(response_iovecs,
                                                                     response_serialized.size(),
                                                                     RPC_MAX_BODY_SIZE,
                                                                     parsed_request);

    RpcResponse parsed_response;
    auto response_result = galay::rpc::detail::try_parse_response_message(request_iovecs,
                                                                       request_serialized.size(),
                                                                       RPC_MAX_BODY_SIZE,
                                                                       parsed_response);

    writer.write_test_case("RpcReader rejects wrong request/response message types",
        !request_result.has_value() &&
        request_result.error().code() == RpcErrorCode::INVALID_REQUEST &&
        !response_result.has_value() &&
        response_result.error().code() == RpcErrorCode::INVALID_RESPONSE);
}

void test_unknown_reserved_bits_rejected(test::TestResultWriter& writer) {
    RpcRequest request(1007, "BoundaryService", "reserved");
    auto serialized = request.serialize();
    serialized[7] = static_cast<char>(0x80);
    std::array<iovec, 1> iovecs{iovec{serialized.data(), serialized.size()}};

    RpcRequest parsed_request;
    auto parse_result = galay::rpc::detail::try_parse_request_message(iovecs,
                                                                   serialized.size(),
                                                                   RPC_MAX_BODY_SIZE,
                                                                   parsed_request);
    auto decode_result = RpcCodec::decode_request(serialized.data(), serialized.size());

    writer.write_test_case("RpcReader rejects unknown request reserved bits",
        !parse_result.has_value() &&
        parse_result.error().code() == RpcErrorCode::INVALID_REQUEST &&
        !decode_result.has_value() &&
        decode_result.error().code() == RpcErrorCode::INVALID_REQUEST);
}

int main() {
    test::TestResultWriter writer("t5_reader_writer_boundary.result");

    std::cout << "Running RPC Reader/Writer Boundary Tests...\n";

    test_request_payload_split_across_iovecs(writer);
    test_request_metadata_split_across_iovecs(writer);
    test_response_payload_split_across_iovecs(writer);
    test_reader_setting_rejects_large_message(writer);
    test_wrong_message_type_rejected(writer);
    test_unknown_reserved_bits_rejected(writer);

    writer.write_summary();

    std::cout << "Tests completed. Passed: " << writer.passed()
              << ", Failed: " << writer.failed() << "\n";

    return writer.failed() > 0 ? 1 : 0;
}
