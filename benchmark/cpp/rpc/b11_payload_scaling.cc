#include <galay/cpp/galay-rpc/kernel/rpc_conn.h>
#include <galay/cpp/galay-rpc/kernel/rpc_stream.h>
#include <galay/cpp/galay-rpc/protoc/rpc_message.h>

#include <array>
#include <chrono>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

using namespace galay::rpc;

int main(int argc, char** argv)
{
    const size_t requests = argc > 1 ? static_cast<size_t>(std::stoull(argv[1])) : 10000;
    const size_t payload_size = argc > 2 ? static_cast<size_t>(std::stoull(argv[2])) : 1024;
    std::string payload(payload_size, 'x');
    size_t bytes = 0;
    size_t errors = 0;
    size_t rejected_boundaries = 0;
    const auto start = std::chrono::steady_clock::now();
    for (size_t i = 0; i < requests; ++i) {
        RpcRequest request(static_cast<uint32_t>(i + 1), "BenchService", "echo");
        request.payload(payload.data(), payload.size());
        auto wire = request.serialize();
        if (wire.size() < RPC_HEADER_SIZE) {
            ++errors;
            continue;
        }
        auto decoded = RpcRequest{};
        if (!decoded.deserializeBody(wire.data() + RPC_HEADER_SIZE, wire.size() - RPC_HEADER_SIZE)) {
            ++errors;
        }
        bytes += wire.size();
    }

    for (size_t i = 0; i < requests; ++i) {
        std::array<char, RPC_HEADER_SIZE> header_buf{};
        RpcHeader heartbeat;
        heartbeat.m_type = static_cast<uint8_t>(RpcMessageType::HEARTBEAT);
        heartbeat.m_body_length = 1;
        heartbeat.serialize(header_buf.data());
        RpcHeader parsed;
        if (!parsed.deserialize(header_buf.data())) {
            ++rejected_boundaries;
        } else {
            ++errors;
        }

        RpcRequest request(static_cast<uint32_t>(i + 1), "BenchService", "echo");
        request.payloadView(RpcPayloadView{"x", RPC_MAX_BODY_SIZE, "y", 1});
        galay::rpc::detail::RpcRequestWriteState request_state(request);
        if (!request_state.takeResult().has_value()) {
            ++rejected_boundaries;
        } else {
            ++errors;
        }

        RpcRequest overflow_request(static_cast<uint32_t>(i + 1), "BenchService", "echo");
        overflow_request.payloadView(RpcPayloadView{
            "x",
            std::numeric_limits<size_t>::max(),
            "y",
            1});
        galay::rpc::detail::RpcRequestWriteState overflow_request_state(overflow_request);
        if (!overflow_request_state.takeResult().has_value()) {
            ++rejected_boundaries;
        } else {
            ++errors;
        }

        RpcResponse response(static_cast<uint32_t>(i + 1), RpcErrorCode::OK);
        response.payloadView(RpcPayloadView{"x", RPC_MAX_BODY_SIZE - 1, "y", 1});
        galay::rpc::detail::RpcResponseWriteState response_state(response);
        if (!response_state.takeResult().has_value()) {
            ++rejected_boundaries;
        } else {
            ++errors;
        }

        galay::rpc::detail::StreamFrameWriteState stream_state(
            static_cast<uint32_t>(i + 1),
            RpcPayloadView{"x", RPC_MAX_BODY_SIZE, "y", 1});
        if (!stream_state.takeResult().has_value()) {
            ++rejected_boundaries;
        } else {
            ++errors;
        }
    }

    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::cout << "RPC payload scaling\nrequests=" << requests
              << "\npayload=" << payload_size
              << "\nqps=" << (seconds > 0.0 ? static_cast<double>(requests) / seconds : 0.0)
              << "\nbytes_per_sec=" << (seconds > 0.0 ? static_cast<double>(bytes) / seconds : 0.0)
              << "\nboundary_rejections=" << rejected_boundaries
              << "\nerrors=" << errors << "\n";
    return errors == 0 ? 0 : 1;
}
