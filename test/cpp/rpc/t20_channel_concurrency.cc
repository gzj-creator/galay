#include "result_writer.h"

#include <galay/cpp/galay-rpc/kernel/rpc_channel.h>

#include <string>

using namespace galay::rpc;

namespace {

void expect(test::TestResultWriter& writer, const std::string& name, bool passed) {
    writer.write_test_case(name, passed);
}

bool payload_equals(const RpcResponse& response, const std::string& expected) {
    const auto& payload = response.payload();
    return std::string(payload.begin(), payload.end()) == expected;
}

std::optional<RpcCallResult> take_ready_result(RpcChannelPendingCall& pending) {
    auto awaitable = pending.waiter.wait();
    if (!awaitable.await_ready()) {
        return std::nullopt;
    }
    auto result = awaitable.await_resume();
    if (!result.has_value()) {
        return RpcCallResult(std::unexpected(RpcError::from(result.error())));
    }
    return std::move(result.value());
}

}  // namespace

int main() {
    test::TestResultWriter writer("rpc_t20_channel_concurrency_results.txt");

    RpcChannelOptions options;
    options.max_in_flight = 2;
    RpcChannelState channel(options);

    auto first = channel.register_pending(10);
    auto second = channel.register_pending(20);
    auto third = channel.register_pending(30);

    expect(writer, "channel accepts pending calls up to max in-flight",
           first.has_value() && second.has_value() && channel.pending_count() == 2);
    expect(writer, "channel rejects calls above max in-flight",
           !third.has_value() && third.error().code() == RpcErrorCode::RESOURCE_EXHAUSTED);

    RpcResponse response20(20, RpcErrorCode::OK);
    response20.payload("twenty", 6);
    RpcResponse response10(10, RpcErrorCode::OK);
    response10.payload("ten", 3);

    auto dispatch20 = channel.dispatch_response(std::move(response20));
    auto dispatch10 = channel.dispatch_response(std::move(response10));
    auto first_result = take_ready_result(*first.value());
    auto second_result = take_ready_result(*second.value());

    expect(writer, "out-of-order response dispatches to the matching pending waiter",
           dispatch20.has_value() &&
           dispatch10.has_value() &&
           first_result.has_value() &&
           first_result->has_value() &&
           first_result->value().has_value() &&
           first_result->value()->request_id() == 10 &&
           payload_equals(*first_result->value(), "ten") &&
           second_result.has_value() &&
           second_result->has_value() &&
           second_result->value().has_value() &&
           second_result->value()->request_id() == 20 &&
           payload_equals(*second_result->value(), "twenty") &&
           channel.pending_count() == 0);

    auto timeout_pending = channel.register_pending(40);
    auto timeout_result = channel.fail_pending(40, RpcError(RpcErrorCode::DEADLINE_EXCEEDED));
    auto timeout_wait = take_ready_result(*timeout_pending.value());
    expect(writer, "timeout removes pending entry",
           timeout_pending.has_value() &&
           timeout_result &&
           timeout_wait.has_value() &&
           !timeout_wait->has_value() &&
           timeout_wait->error().code() == RpcErrorCode::DEADLINE_EXCEEDED &&
           channel.pending_count() == 0);

    auto close_first = channel.register_pending(50);
    auto close_second = channel.register_pending(60);
    auto close_failures = channel.fail_all_pending(RpcError(RpcErrorCode::UNAVAILABLE));
    auto close_first_wait = take_ready_result(*close_first.value());
    auto close_second_wait = take_ready_result(*close_second.value());
    expect(writer, "connection close fails all pending calls exactly once",
           close_failures == 2 &&
           close_first_wait.has_value() &&
           !close_first_wait->has_value() &&
           close_first_wait->error().code() == RpcErrorCode::UNAVAILABLE &&
           close_second_wait.has_value() &&
           !close_second_wait->has_value() &&
           close_second_wait->error().code() == RpcErrorCode::UNAVAILABLE &&
           channel.pending_count() == 0 &&
           channel.fail_all_pending(RpcError(RpcErrorCode::UNAVAILABLE)) == 0);

    writer.write_summary();
    return writer.failed() == 0 ? 0 : 1;
}
