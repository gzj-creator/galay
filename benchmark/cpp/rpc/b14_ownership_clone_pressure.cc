#include <galay/cpp/galay-rpc/kernel/rpc_conn.h>
#include <galay/cpp/galay-rpc/kernel/rpc_stream.h>
#include <galay/cpp/galay-rpc/protoc/rpc_message.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

using namespace galay::rpc;

namespace {

size_t parseSizeArg(char** argv, int index, size_t fallback)
{
    if (argv[index] == nullptr) {
        return fallback;
    }
    char* end = nullptr;
    const auto value = std::strtoull(argv[index], &end, 10);
    if (end == argv[index] || *end != '\0') {
        return fallback;
    }
    return static_cast<size_t>(value);
}

bool viewEquals(RpcPayloadView view, std::string_view expected)
{
    if (view.size() != expected.size()) {
        return false;
    }
    if (view.segment1_len > 0 &&
        std::memcmp(view.segment1, expected.data(), view.segment1_len) != 0) {
        return false;
    }
    if (view.segment2_len > 0 &&
        std::memcmp(view.segment2,
                    expected.data() + view.segment1_len,
                    view.segment2_len) != 0) {
        return false;
    }
    return true;
}

} // namespace

int main(int argc, char** argv)
{
    const size_t iterations = argc > 1 ? parseSizeArg(argv, 1, 10000) : 10000;
    const size_t payload_size = argc > 2 ? parseSizeArg(argv, 2, 1024) : 1024;

    std::string owned_payload(payload_size, 'x');
    std::string borrowed_first(payload_size / 2, 'a');
    std::string borrowed_second(payload_size - borrowed_first.size(), 'b');
    const std::string borrowed_payload = borrowed_first + borrowed_second;
    const RpcPayloadView borrowed_view{
        borrowed_first.data(),
        borrowed_first.size(),
        borrowed_second.data(),
        borrowed_second.size()
    };

    RpcRequest owned_request(1, "BenchOwnershipService", "clone");
    owned_request.payload(owned_payload.data(), owned_payload.size());
    RpcRequest borrowed_request(2, "BenchOwnershipService", "clone");
    borrowed_request.payloadView(borrowed_view);

    RpcResponse owned_response(3, RpcErrorCode::OK);
    owned_response.payload(owned_payload.data(), owned_payload.size());
    RpcResponse borrowed_response(4, RpcErrorCode::OK);
    borrowed_response.payloadView(borrowed_view);

    StreamMessage owned_message(5, owned_payload.data(), owned_payload.size());
    StreamMessage borrowed_message;
    borrowed_message.streamId(6);
    borrowed_message.payloadView(borrowed_view);

    size_t checksum = 0;
    size_t errors = 0;

    const auto clone_start = std::chrono::steady_clock::now();
    for (size_t i = 0; i < iterations; ++i) {
        RpcRequest owned_request_clone = owned_request.clone();
        RpcRequest borrowed_request_clone = borrowed_request.clone();
        RpcResponse owned_response_clone = owned_response.clone();
        RpcResponse borrowed_response_clone = borrowed_response.clone();
        StreamMessage owned_message_clone = owned_message.clone();
        StreamMessage borrowed_message_clone = borrowed_message.clone();

        checksum += owned_request_clone.payloadSize();
        checksum += borrowed_request_clone.payloadSize();
        checksum += owned_response_clone.payloadSize();
        checksum += borrowed_response_clone.payloadSize();
        checksum += owned_message_clone.payloadSize();
        checksum += borrowed_message_clone.payloadSize();

        if (!viewEquals(owned_request_clone.payloadView(), owned_payload) ||
            !viewEquals(borrowed_request_clone.payloadView(), borrowed_payload) ||
            !viewEquals(owned_response_clone.payloadView(), owned_payload) ||
            !viewEquals(borrowed_response_clone.payloadView(), borrowed_payload) ||
            !viewEquals(owned_message_clone.payloadView(), owned_payload) ||
            !viewEquals(borrowed_message_clone.payloadView(), borrowed_payload)) {
            ++errors;
        }
    }
    const auto clone_end = std::chrono::steady_clock::now();

    const auto write_state_start = std::chrono::steady_clock::now();
    for (size_t i = 0; i < iterations; ++i) {
        galay::rpc::detail::RpcRequestWriteState request_state(owned_request);
        galay::rpc::detail::RpcResponseWriteState response_state(owned_response);
        galay::rpc::detail::StreamFrameWriteState stream_state(owned_message.streamId(), borrowed_view);
        std::vector<char> raw_payload(owned_payload.begin(), owned_payload.end());
        galay::rpc::detail::RpcVectorWriteState vector_state(std::move(raw_payload));

        checksum += request_state.writeIovecsCount();
        checksum += response_state.writeIovecsCount();
        checksum += stream_state.writeIovecsCount();
        checksum += vector_state.writeIovecsCount();

        if (!request_state.takeResult().has_value() ||
            !response_state.takeResult().has_value() ||
            !stream_state.takeResult().has_value() ||
            !vector_state.takeResult().has_value()) {
            ++errors;
        }
    }
    const auto write_state_end = std::chrono::steady_clock::now();

    const double clone_seconds =
        std::chrono::duration<double>(clone_end - clone_start).count();
    const double write_state_seconds =
        std::chrono::duration<double>(write_state_end - write_state_start).count();
    const size_t clone_ops = iterations * 6;
    const size_t write_state_ops = iterations * 4;

    std::cout << "RPC ownership clone pressure\n"
              << "iterations=" << iterations
              << "\npayload=" << payload_size
              << "\nclone_ops=" << clone_ops
              << "\nclone_ops_per_sec="
              << (clone_seconds > 0.0 ? static_cast<double>(clone_ops) / clone_seconds : 0.0)
              << "\nwrite_state_ops=" << write_state_ops
              << "\nwrite_state_ops_per_sec="
              << (write_state_seconds > 0.0 ? static_cast<double>(write_state_ops) / write_state_seconds : 0.0)
              << "\nchecksum=" << checksum
              << "\nerrors=" << errors << "\n";

    return errors == 0 ? 0 : 1;
}
