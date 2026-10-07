#include <galay/cpp/galay-rpc/kernel/rpc_stream.h>
#include <galay/cpp/galay-rpc/kernel/streamsvc.h>

#include <iostream>
#include <vector>

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

std::vector<char> concat(std::vector<char> first, const std::vector<char>& second)
{
    first.insert(first.end(), second.begin(), second.end());
    return first;
}

int expect_control_frame_body_rejected(RpcMessageType type, const char* name)
{
    StreamMessage invalid_control(9, "bad", 3);
    auto invalid_frame = invalid_control.serialize(type);
    StreamMessage next_data(9, "ok", 2);
    auto next_frame = next_data.serialize(RpcMessageType::STREAM_DATA);
    auto wire = concat(std::move(invalid_frame), next_frame);

    RingBuffer<galay::utils::RingBufferBackendStrategy::Mmap, std::dynamic_extent> ring_buffer(128);
    if (auto rc = expect(
            ring_buffer.try_write_batch(wire.data(), wire.size()) == wire.size(),
                         "failed to seed stream ring buffer")) {
        return rc;
    }

    StreamMessage parsed;
    galay::rpc::detail::StreamMessageReadState state(ring_buffer, parsed);
    const bool completed = state.parse_from_ring_buffer();
    auto result = state.take_result();
    if (auto rc = expect(completed &&
                             !result.has_value() &&
                             result.error().code() == RpcErrorCode::INVALID_REQUEST,
                         name)) {
        return rc;
    }
    if (auto rc = expect(ring_buffer.readable() == next_frame.size(),
                         "invalid stream control body polluted the next frame")) {
        return rc;
    }

    StreamMessage next;
    galay::rpc::detail::StreamMessageReadState next_state(ring_buffer, next);
    const bool next_completed = next_state.parse_from_ring_buffer();
    auto next_result = next_state.take_result();
    return expect(next_completed &&
                      next_result.has_value() &&
                      next.message_type() == RpcMessageType::STREAM_DATA &&
                      next.payload_str() == "ok",
                  "stream reader did not recover after rejecting control body");
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
    auto built = builder.max_frame_bytes(4).build_config();
    if (auto rc = expect(built.stream_limits.max_frame_bytes == 4,
                         "builder did not apply stream frame limit")) {
        return rc;
    }

    StreamMessage init(7, "svc", 3);
    init.message_type(RpcMessageType::STREAM_INIT);
    if (auto rc = expect(init.message_type() == RpcMessageType::STREAM_INIT,
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
    cancel.message_type(RpcMessageType::STREAM_CANCEL);
    auto first_cancel = cancel.serialize(RpcMessageType::STREAM_CANCEL);
    auto second_cancel = cancel.serialize(RpcMessageType::STREAM_CANCEL);
    if (auto rc = expect(first_cancel == second_cancel,
                         "stream cancel serialization was not idempotent")) {
        return rc;
    }

    StreamMessage overlapping_init(8, "svc", 3);
    overlapping_init.message_type(RpcMessageType::STREAM_INIT);
    auto deterministic_reject = overlapping_init.serialize(RpcMessageType::STREAM_CANCEL);
    if (auto rc = expect(deterministic_reject.size() == RPC_HEADER_SIZE + overlapping_init.payload_size(),
                         "overlapping init rejection frame was not deterministic")) {
        return rc;
    }

    if (auto rc = expect_control_frame_body_rejected(RpcMessageType::STREAM_END,
                                                     "stream end with body was not rejected")) {
        return rc;
    }
    if (auto rc = expect_control_frame_body_rejected(RpcMessageType::STREAM_CANCEL,
                                                     "stream cancel with body was not rejected")) {
        return rc;
    }

    std::cout << "RPC stream contract PASS\n";
    return 0;
}
