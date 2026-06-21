#include <galay/cpp/galay-rpc/kernel/rpc_stream.h>
#include <galay/cpp/galay-rpc/kernel/streamsvc.h>

#include <iostream>

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
    RpcStreamServerConfig config;
    if (auto rc = expect(config.stream_limits.max_frame_bytes == RPC_MAX_BODY_SIZE,
                         "default stream frame limit changed")) {
        return rc;
    }

    RpcStreamServerBuilder builder;
    auto built = builder.maxFrameBytes(4).buildConfig();
    if (auto rc = expect(built.stream_limits.max_frame_bytes == 4,
                         "builder did not apply stream frame limit")) {
        return rc;
    }

    StreamMessage init(7, "svc", 3);
    init.messageType(RpcMessageType::STREAM_INIT);
    if (auto rc = expect(init.messageType() == RpcMessageType::STREAM_INIT,
                         "stream init type was not stable")) {
        return rc;
    }

    RpcStreamLimits limits;
    limits.max_frame_bytes = 4;
    StreamMessage oversized(7, "12345", 5);
    auto rejected = oversized.validate(limits);
    if (auto rc = expect(!rejected.has_value() &&
                             rejected.error().code() == RpcErrorCode::RESOURCE_EXHAUSTED,
                         "oversized stream frame was not rejected deterministically")) {
        return rc;
    }

    StreamMessage cancel(7, nullptr, 0);
    cancel.messageType(RpcMessageType::STREAM_CANCEL);
    auto first_cancel = cancel.serialize(RpcMessageType::STREAM_CANCEL);
    auto second_cancel = cancel.serialize(RpcMessageType::STREAM_CANCEL);
    if (auto rc = expect(first_cancel == second_cancel,
                         "stream cancel serialization was not idempotent")) {
        return rc;
    }

    StreamMessage overlapping_init(8, "svc", 3);
    overlapping_init.messageType(RpcMessageType::STREAM_INIT);
    auto deterministic_reject = overlapping_init.serialize(RpcMessageType::STREAM_CANCEL);
    if (auto rc = expect(deterministic_reject.size() == RPC_HEADER_SIZE + overlapping_init.payloadSize(),
                         "overlapping init rejection frame was not deterministic")) {
        return rc;
    }

    std::cout << "RPC stream contract PASS\n";
    return 0;
}
