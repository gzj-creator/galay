#include <galay/cpp/galay-rpc/kernel/rpc_channel.h>
#include <galay/cpp/galay-rpc/kernel/rpc_conn.h>
#include <galay/cpp/galay-rpc/kernel/rpc_stream.h>
#include <galay/cpp/galay-rpc/protoc/rpc_message.h>

#include <array>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string>

using namespace galay::rpc;

namespace {

int expect(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << message << "\n";
        return 1;
    }
    return 0;
}

bool writeStateFailed(galay::rpc::detail::RpcRequestWriteState& state)
{
    auto result = state.takeResult();
    return !result.has_value() &&
           result.error().code() == RpcErrorCode::INVALID_REQUEST;
}

bool writeStateFailed(galay::rpc::detail::RpcResponseWriteState& state)
{
    auto result = state.takeResult();
    return !result.has_value() &&
           result.error().code() == RpcErrorCode::INVALID_RESPONSE;
}

bool writeStateFailed(galay::rpc::detail::StreamFrameWriteState& state)
{
    auto result = state.takeResult();
    return !result.has_value() &&
           result.error().code() == RpcErrorCode::INVALID_REQUEST;
}

} // namespace

int main()
{
    std::array<char, RPC_HEADER_SIZE> header_buf{};
    RpcHeader heartbeat_with_body;
    heartbeat_with_body.m_type = static_cast<uint8_t>(RpcMessageType::HEARTBEAT);
    heartbeat_with_body.m_body_length = 1;
    heartbeat_with_body.serialize(header_buf.data());

    RpcHeader parsed_heartbeat;
    if (auto rc = expect(!parsed_heartbeat.deserialize(header_buf.data()),
                         "heartbeat with body was accepted")) {
        return rc;
    }

    RpcRequest long_service(1,
                            std::string(static_cast<size_t>(std::numeric_limits<uint16_t>::max()) + 1U, 's'),
                            "method");
    galay::rpc::detail::RpcRequestWriteState long_service_state(long_service);
    if (auto rc = expect(writeStateFailed(long_service_state),
                         "request writer accepted service length beyond uint16")) {
        return rc;
    }

    RpcRequest long_method(2,
                           "service",
                           std::string(static_cast<size_t>(std::numeric_limits<uint16_t>::max()) + 1U, 'm'));
    galay::rpc::detail::RpcRequestWriteState long_method_state(long_method);
    if (auto rc = expect(writeStateFailed(long_method_state),
                         "request writer accepted method length beyond uint16")) {
        return rc;
    }

    RpcRequest oversized_request(3, "service", "method");
    oversized_request.payloadView(RpcPayloadView{"x", RPC_MAX_BODY_SIZE, "y", 1});
    galay::rpc::detail::RpcRequestWriteState oversized_request_state(oversized_request);
    if (auto rc = expect(writeStateFailed(oversized_request_state),
                         "request writer accepted body beyond RPC_MAX_BODY_SIZE")) {
        return rc;
    }

    RpcRequest overflow_request(30, "service", "method");
    overflow_request.payloadView(RpcPayloadView{
        "x",
        std::numeric_limits<size_t>::max(),
        "y",
        1});
    galay::rpc::detail::RpcRequestWriteState overflow_request_state(overflow_request);
    if (auto rc = expect(writeStateFailed(overflow_request_state),
                         "request writer accepted overflowing payload length")) {
        return rc;
    }

    RpcResponse oversized_response(4, RpcErrorCode::OK);
    oversized_response.payloadView(RpcPayloadView{"x", RPC_MAX_BODY_SIZE - 1, "y", 1});
    galay::rpc::detail::RpcResponseWriteState oversized_response_state(oversized_response);
    if (auto rc = expect(writeStateFailed(oversized_response_state),
                         "response writer accepted body beyond RPC_MAX_BODY_SIZE")) {
        return rc;
    }

    galay::rpc::detail::StreamFrameWriteState oversized_stream_data(
        5,
        RpcPayloadView{"x", RPC_MAX_BODY_SIZE, "y", 1});
    if (auto rc = expect(writeStateFailed(oversized_stream_data),
                         "stream writer accepted data body beyond RPC_MAX_BODY_SIZE")) {
        return rc;
    }

    galay::rpc::detail::StreamFrameWriteState overflow_stream_data(
        50,
        RpcPayloadView{"x", std::numeric_limits<size_t>::max(), "y", 1});
    if (auto rc = expect(writeStateFailed(overflow_stream_data),
                         "stream writer accepted overflowing payload length")) {
        return rc;
    }

    galay::rpc::detail::StreamFrameWriteState long_stream_init(
        6,
        std::string(static_cast<size_t>(std::numeric_limits<uint16_t>::max()) + 1U, 's'),
        "method");
    if (auto rc = expect(writeStateFailed(long_stream_init),
                         "stream init writer accepted service length beyond uint16")) {
        return rc;
    }

    RpcChannelState state;
    auto pending = state.registerPending(7);
    if (auto rc = expect(pending.has_value(), "pending registration failed")) {
        return rc;
    }

    auto first = state.failPending(7, RpcError(RpcErrorCode::CANCELLED, "cancelled"));
    auto second = state.failPending(7, RpcError(RpcErrorCode::UNAVAILABLE, "closed"));
    if (auto rc = expect(first &&
                             !second &&
                             state.pendingCount() == 0,
                         "cancel/close cleanup produced a dangling or double pending wakeup")) {
        return rc;
    }

    std::cout << "RPC Task 4 boundaries PASS\n";
    return 0;
}
