#include <galay/cpp/galay-rpc/kernel/rpc_channel.h>
#include <galay/cpp/galay-rpc/kernel/rpc_stream.h>

#include <iostream>
#include <memory>

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

} // namespace

int main()
{
    RpcChannelOptions options;
    options.max_in_flight = 1;
    options.max_outbound_queue = 1;
    options.max_outbound_bytes = 8;

    RpcChannelState state(options);
    auto pending_a = state.registerPending(1);
    if (auto rc = expect(pending_a.has_value(), "first pending registration failed")) {
        return rc;
    }
    auto pending_b = state.registerPending(2);
    if (auto rc = expect(!pending_b.has_value() &&
                             pending_b.error().code() == RpcErrorCode::RESOURCE_EXHAUSTED,
                         "in-flight limit did not return RESOURCE_EXHAUSTED")) {
        return rc;
    }
    state.failPending(1, RpcError(RpcErrorCode::CANCELLED, "cleanup"));
    auto pending_c = state.registerPending(3);
    if (auto rc = expect(pending_c.has_value(), "in-flight exhaustion poisoned later registration")) {
        return rc;
    }

    RpcOutboundBackpressure outbound(options);
    auto small = outbound.reserve(4);
    if (auto rc = expect(small.has_value(), "small outbound reservation failed")) {
        return rc;
    }
    auto too_many_items = outbound.reserve(1);
    if (auto rc = expect(!too_many_items.has_value() &&
                             too_many_items.error().code() == RpcErrorCode::RESOURCE_EXHAUSTED,
                         "outbound queue item limit did not reject")) {
        return rc;
    }
    outbound.release(4);
    auto too_many_bytes = outbound.reserve(9);
    if (auto rc = expect(!too_many_bytes.has_value() &&
                             too_many_bytes.error().code() == RpcErrorCode::RESOURCE_EXHAUSTED,
                         "outbound byte limit did not reject")) {
        return rc;
    }
    auto after_reject = outbound.reserve(8);
    if (auto rc = expect(after_reject.has_value(), "outbound rejection poisoned later valid reservation")) {
        return rc;
    }

    RpcStreamLimits stream_limits;
    stream_limits.max_frame_bytes = 4;
    StreamMessage frame(7, "12345", 5);
    auto oversized = frame.validate(stream_limits);
    if (auto rc = expect(!oversized.has_value() &&
                             oversized.error().code() == RpcErrorCode::RESOURCE_EXHAUSTED,
                         "stream frame limit did not reject oversized payload")) {
        return rc;
    }
    frame.payload("1234");
    auto valid = frame.validate(stream_limits);
    if (auto rc = expect(valid.has_value(), "stream frame limit poisoned later valid frame")) {
        return rc;
    }

    std::cout << "RPC backpressure PASS\n";
    return 0;
}
