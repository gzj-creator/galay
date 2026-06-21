#include <galay/cpp/galay-rpc/kernel/rpc_call.h>
#include <galay/cpp/galay-rpc/kernel/rpc_client.h>
#include <galay/cpp/galay-rpc/kernel/rpc_server.h>
#include <galay/cpp/galay-rpc/kernel/rpc_service.h>
#include <galay/cpp/galay-kernel/common/sleep.hpp>
#include <galay/cpp/galay-kernel/core/runtime.h>

#include <atomic>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <unistd.h>

using namespace galay::kernel;
using namespace galay::rpc;

namespace {

class DeadlineCancelService final : public RpcService {
public:
    DeadlineCancelService()
        : RpcService("DeadlineCancelService")
    {
        registerMethod("slow", &DeadlineCancelService::slow);
        registerMethod("echo", &DeadlineCancelService::echo);
    }

    Task<void> slow(RpcContext& ctx)
    {
        co_await sleep(std::chrono::milliseconds(80));
        ctx.setPayload(ctx.request().payloadView());
        co_return;
    }

    Task<void> echo(RpcContext& ctx)
    {
        ctx.setPayload(ctx.request().payloadView());
        co_return;
    }
};

struct TestState {
    std::atomic<bool> done{false};
    bool ok = true;
    std::string error;
};

uint16_t loopbackPort()
{
    return static_cast<uint16_t>(30000 + (::getpid() % 12000));
}

Task<void> cancelSoon(RpcCancellationSource* source);
Task<void> closeSoon(RpcClient* client, std::atomic<bool>* closed);

template<typename AwaitResult>
RpcErrorCode resultCode(const AwaitResult& result)
{
    if (!result.has_value()) {
        return RpcErrorCode::INTERNAL_ERROR;
    }
    const auto& call_result = result.value();
    if (!call_result.has_value()) {
        return call_result.error().code();
    }
    if (!call_result->has_value()) {
        return RpcErrorCode::UNKNOWN_ERROR;
    }
    return call_result->value().errorCode();
}

template<typename AwaitResult>
bool payloadEquals(const AwaitResult& result, const std::string& expected)
{
    if (!result.has_value()) {
        return false;
    }
    const auto& call_result = result.value();
    if (!call_result.has_value() || !call_result->has_value()) {
        return false;
    }
    const auto& payload = call_result->value().payload();
    return call_result->value().isOk() &&
           std::string(payload.begin(), payload.end()) == expected;
}

void fail(TestState& state, std::string message)
{
    state.ok = false;
    state.error = std::move(message);
}

Task<void> runDeadlineCancelClient(uint16_t port, TestState* state)
{
    RpcClient client;
    bool connected = false;
    for (int attempt = 0; attempt < 100; ++attempt) {
        auto connect_result = co_await client.connect("127.0.0.1", port);
        if (connect_result.has_value()) {
            connected = true;
            break;
        }
        co_await sleep(std::chrono::milliseconds(10));
    }
    if (!connected) {
        fail(*state, "client connect retry exhausted");
        state->done.store(true, std::memory_order_release);
        co_return;
    }

    RpcCallOptions deadline_options;
    deadline_options.timeout(std::chrono::milliseconds(10));
    auto deadline_result = co_await client.call("DeadlineCancelService", "slow", "late", deadline_options);
    if (resultCode(deadline_result) != RpcErrorCode::DEADLINE_EXCEEDED) {
        fail(*state, "slow call did not return DEADLINE_EXCEEDED");
        co_await client.close();
        state->done.store(true, std::memory_order_release);
        co_return;
    }

    RpcCancellationSource pre_cancel_source;
    pre_cancel_source.cancel();
    RpcCallOptions pre_cancel_options;
    pre_cancel_options.cancellationToken(pre_cancel_source.token());
    auto pre_cancel_result = co_await client.call("DeadlineCancelService", "echo", "pre", pre_cancel_options);
    if (resultCode(pre_cancel_result) != RpcErrorCode::CANCELLED) {
        fail(*state, "pre-cancelled call did not return CANCELLED");
        co_await client.close();
        state->done.store(true, std::memory_order_release);
        co_return;
    }

    RpcCancellationSource pending_cancel_source;
    RpcCallOptions pending_cancel_options;
    pending_cancel_options.cancellationToken(pending_cancel_source.token());
    auto runtime = RuntimeHandle::current();
    if (!runtime.has_value()) {
        fail(*state, "runtime handle unavailable");
        co_await client.close();
        state->done.store(true, std::memory_order_release);
        co_return;
    }

    auto canceller = runtime->spawn(cancelSoon(&pending_cancel_source));
    if (!canceller.has_value()) {
        fail(*state, "failed to schedule canceller");
        co_await client.close();
        state->done.store(true, std::memory_order_release);
        co_return;
    }
    auto pending_cancel_result = co_await client.call("DeadlineCancelService", "slow", "cancel", pending_cancel_options);
    if (resultCode(pending_cancel_result) != RpcErrorCode::CANCELLED) {
        fail(*state, "pending call did not return CANCELLED");
        co_await client.close();
        state->done.store(true, std::memory_order_release);
        co_return;
    }

    co_await sleep(std::chrono::milliseconds(120));
    auto echo_result = co_await client.call("DeadlineCancelService", "echo", "after-late");
    if (!payloadEquals(echo_result, "after-late")) {
        fail(*state, "late response corrupted later call");
        co_await client.close();
        state->done.store(true, std::memory_order_release);
        co_return;
    }

    std::atomic<bool> close_done{false};
    auto closer = runtime->spawn(closeSoon(&client, &close_done));
    if (!closer.has_value()) {
        fail(*state, "failed to schedule close watcher");
        co_await client.close();
        state->done.store(true, std::memory_order_release);
        co_return;
    }
    RpcCancellationSource close_cancel_source;
    RpcCallOptions close_cancel_options;
    close_cancel_options.cancellationToken(close_cancel_source.token());
    auto close_pending_result = co_await client.call("DeadlineCancelService", "slow", "close", close_cancel_options);
    for (int i = 0; i < 100 && !close_done.load(std::memory_order_acquire); ++i) {
        co_await sleep(std::chrono::milliseconds(1));
    }
    if (resultCode(close_pending_result) != RpcErrorCode::UNAVAILABLE ||
        !close_done.load(std::memory_order_acquire)) {
        fail(*state, "close did not drain pending cancellable call");
        state->done.store(true, std::memory_order_release);
        co_return;
    }

    co_await client.close();
    state->done.store(true, std::memory_order_release);
    co_return;
}

Task<void> cancelSoon(RpcCancellationSource* source)
{
    co_await sleep(std::chrono::milliseconds(10));
    source->cancel();
    co_return;
}

Task<void> closeSoon(RpcClient* client, std::atomic<bool>* closed)
{
    co_await sleep(std::chrono::milliseconds(10));
    auto close_result = co_await client->close();
    (void)close_result;
    closed->store(true, std::memory_order_release);
    co_return;
}

} // namespace

int main()
{
    const uint16_t port = loopbackPort();

    auto server = RpcServerBuilder()
        .host("127.0.0.1")
        .port(port)
        .ioSchedulerCount(1)
        .computeSchedulerCount(0)
        .build();
    server.registerService(std::make_shared<DeadlineCancelService>());
    server.start();

    Runtime runtime = RuntimeBuilder().ioSchedulerCount(1).computeSchedulerCount(0).build();
    runtime.start();

    TestState state;
    auto root = runtime.spawn(runDeadlineCancelClient(port, &state));
    if (!root.has_value()) {
        runtime.stop();
        server.stop();
        std::cerr << "failed to schedule deadline cancel client\n";
        return 1;
    }

    for (int i = 0; i < 600 && !state.done.load(std::memory_order_acquire); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    runtime.stop();
    server.stop();

    if (!state.done.load(std::memory_order_acquire)) {
        std::cerr << "deadline/cancel test timed out\n";
        return 1;
    }
    if (!state.ok) {
        std::cerr << state.error << "\n";
        return 1;
    }

    std::cout << "RPC deadline/cancel PASS\n";
    return 0;
}
