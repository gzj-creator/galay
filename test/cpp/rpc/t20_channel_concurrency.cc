#include "result_writer.h"

#include <galay/cpp/galay-rpc/kernel/rpc_channel.h>

#include <string>

using namespace galay::rpc;

namespace {

void expect(test::TestResultWriter& writer, const std::string& name, bool passed) {
    writer.writeTestCase(name, passed);
}

bool payloadEquals(const RpcResponse& response, const std::string& expected) {
    const auto& payload = response.payload();
    return std::string(payload.begin(), payload.end()) == expected;
}

std::optional<RpcCallResult> takeReadyResult(RpcChannelPendingCall& pending) {
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

    auto first = channel.registerPending(10);
    auto second = channel.registerPending(20);
    auto third = channel.registerPending(30);

    expect(writer, "channel accepts pending calls up to max in-flight",
           first.has_value() && second.has_value() && channel.pendingCount() == 2);
    expect(writer, "channel rejects calls above max in-flight",
           !third.has_value() && third.error().code() == RpcErrorCode::RESOURCE_EXHAUSTED);

    RpcResponse response20(20, RpcErrorCode::OK);
    response20.payload("twenty", 6);
    RpcResponse response10(10, RpcErrorCode::OK);
    response10.payload("ten", 3);

    auto dispatch20 = channel.dispatchResponse(std::move(response20));
    auto dispatch10 = channel.dispatchResponse(std::move(response10));
    auto first_result = takeReadyResult(*first.value());
    auto second_result = takeReadyResult(*second.value());

    expect(writer, "out-of-order response dispatches to the matching pending waiter",
           dispatch20.has_value() &&
           dispatch10.has_value() &&
           first_result.has_value() &&
           first_result->has_value() &&
           first_result->value().has_value() &&
           first_result->value()->requestId() == 10 &&
           payloadEquals(*first_result->value(), "ten") &&
           second_result.has_value() &&
           second_result->has_value() &&
           second_result->value().has_value() &&
           second_result->value()->requestId() == 20 &&
           payloadEquals(*second_result->value(), "twenty") &&
           channel.pendingCount() == 0);

    auto timeout_pending = channel.registerPending(40);
    auto timeout_result = channel.failPending(40, RpcError(RpcErrorCode::DEADLINE_EXCEEDED));
    auto timeout_wait = takeReadyResult(*timeout_pending.value());
    expect(writer, "timeout removes pending entry",
           timeout_pending.has_value() &&
           timeout_result &&
           timeout_wait.has_value() &&
           !timeout_wait->has_value() &&
           timeout_wait->error().code() == RpcErrorCode::DEADLINE_EXCEEDED &&
           channel.pendingCount() == 0);

    auto close_first = channel.registerPending(50);
    auto close_second = channel.registerPending(60);
    auto close_failures = channel.failAllPending(RpcError(RpcErrorCode::UNAVAILABLE));
    auto close_first_wait = takeReadyResult(*close_first.value());
    auto close_second_wait = takeReadyResult(*close_second.value());
    expect(writer, "connection close fails all pending calls exactly once",
           close_failures == 2 &&
           close_first_wait.has_value() &&
           !close_first_wait->has_value() &&
           close_first_wait->error().code() == RpcErrorCode::UNAVAILABLE &&
           close_second_wait.has_value() &&
           !close_second_wait->has_value() &&
           close_second_wait->error().code() == RpcErrorCode::UNAVAILABLE &&
           channel.pendingCount() == 0 &&
           channel.failAllPending(RpcError(RpcErrorCode::UNAVAILABLE)) == 0);

    writer.writeSummary();
    return writer.failed() == 0 ? 0 : 1;
}
