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
        register_method("slow", &DeadlineCancelService::slow);
        register_method("echo", &DeadlineCancelService::echo);
    }

    Task<void> slow(RpcContext& ctx)
    {
        co_await sleep(std::chrono::milliseconds(80));
        ctx.set_payload(ctx.request().payload_view());
        co_return;
    }

    Task<void> echo(RpcContext& ctx)
    {
        ctx.set_payload(ctx.request().payload_view());
        co_return;
    }
};

struct TestState {
    std::atomic<bool> done{false};
    bool ok = true;
    std::string error;
};

uint16_t loopback_port()
{
    return static_cast<uint16_t>(30000 + (::getpid() % 12000));
}

Task<void> cancel_soon(RpcCancellationSource* source);
Task<void> close_soon(RpcClient* client, std::atomic<bool>* closed);

template<typename AwaitResult>
RpcErrorCode result_code(const AwaitResult& result)
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
    return call_result->value().error_code();
}

template<typename AwaitResult>
bool payload_equals(const AwaitResult& result, const std::string& expected)
{
    if (!result.has_value()) {
        return false;
    }
    const auto& call_result = result.value();
    if (!call_result.has_value() || !call_result->has_value()) {
        return false;
    }
    const auto& payload = call_result->value().payload();
    return call_result->value().is_ok() &&
           std::string(payload.begin(), payload.end()) == expected;
}

void fail(TestState& state, std::string message)
{
    state.ok = false;
    state.error = std::move(message);
}

Task<void> run_deadline_cancel_client(uint16_t port, TestState* state)
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
    if (result_code(deadline_result) != RpcErrorCode::DEADLINE_EXCEEDED) {
        fail(*state, "slow call did not return DEADLINE_EXCEEDED");
        co_await client.close();
        state->done.store(true, std::memory_order_release);
        co_return;
    }

    RpcCancellationSource pre_cancel_source;
    pre_cancel_source.cancel();
    RpcCallOptions pre_cancel_options;
    pre_cancel_options.cancellation_token(pre_cancel_source.token());
    auto pre_cancel_result = co_await client.call("DeadlineCancelService", "echo", "pre", pre_cancel_options);
    if (result_code(pre_cancel_result) != RpcErrorCode::CANCELLED) {
        fail(*state, "pre-cancelled call did not return CANCELLED");
        co_await client.close();
        state->done.store(true, std::memory_order_release);
        co_return;
    }

    RpcCancellationSource pending_cancel_source;
    RpcCallOptions pending_cancel_options;
    pending_cancel_options.cancellation_token(pending_cancel_source.token());
    auto runtime = RuntimeHandle::current();
    if (!runtime.has_value()) {
        fail(*state, "runtime handle unavailable");
        co_await client.close();
        state->done.store(true, std::memory_order_release);
        co_return;
    }

    auto* owner = co_await galay::rpc::detail::CurrentSchedulerAwaitable{};
    // Cancellation belongs to the caller's scheduler, not the runtime's next worker.
    if (!schedule_task(owner, cancel_soon(&pending_cancel_source))) {
        fail(*state, "failed to schedule canceller");
        co_await client.close();
        state->done.store(true, std::memory_order_release);
        co_return;
    }
    auto pending_cancel_result = co_await client.call("DeadlineCancelService", "slow", "cancel", pending_cancel_options);
    if (result_code(pending_cancel_result) != RpcErrorCode::CANCELLED) {
        fail(*state, "pending call did not return CANCELLED");
        co_await client.close();
        state->done.store(true, std::memory_order_release);
        co_return;
    }

    co_await sleep(std::chrono::milliseconds(120));
    auto echo_result = co_await client.call("DeadlineCancelService", "echo", "after-late");
    if (!payload_equals(echo_result, "after-late")) {
        fail(*state, "late response corrupted later call");
        co_await client.close();
        state->done.store(true, std::memory_order_release);
        co_return;
    }

    std::atomic<bool> close_done{false};
    auto closer = runtime->spawn_io(close_soon(&client, &close_done));
    if (!closer.has_value()) {
        fail(*state, "failed to schedule close watcher");
        co_await client.close();
        state->done.store(true, std::memory_order_release);
        co_return;
    }
    RpcCancellationSource close_cancel_source;
    RpcCallOptions close_cancel_options;
    close_cancel_options.cancellation_token(close_cancel_source.token());
    auto close_pending_result = co_await client.call("DeadlineCancelService", "slow", "close", close_cancel_options);
    for (int i = 0; i < 100 && !close_done.load(std::memory_order_acquire); ++i) {
        co_await sleep(std::chrono::milliseconds(1));
    }
    if (result_code(close_pending_result) != RpcErrorCode::UNAVAILABLE ||
        !close_done.load(std::memory_order_acquire)) {
        fail(*state, "close did not drain pending cancellable call");
        state->done.store(true, std::memory_order_release);
        co_return;
    }

    co_await client.close();
    state->done.store(true, std::memory_order_release);
    co_return;
}

Task<void> cancel_soon(RpcCancellationSource* source)
{
    co_await sleep(std::chrono::milliseconds(10));
    source->cancel();
    co_return;
}

Task<void> close_soon(RpcClient* client, std::atomic<bool>* closed)
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
    const uint16_t port = loopback_port();

    auto server = RpcServerBuilder()
        .host("127.0.0.1")
        .port(port)
        .io_scheduler_count(1)
        .parallel_scheduler_count(0)
        .build();
    DeadlineCancelService service;
    auto registered = server.register_service(service);
    if (!registered.has_value()) {
        std::cerr << "failed to register deadline service: "
                  << registered.error().message() << "\n";
        return 1;
    }
    auto server_started = server.start();
    if (!server_started.has_value()) {
        std::cerr << "failed to start deadline server: "
                  << server_started.error().message() << "\n";
        return 1;
    }

    Runtime runtime = RuntimeBuilder().io_scheduler_count(2).parallel_scheduler_count(0).build();
    auto runtime_started = runtime.start();
    if (!runtime_started.has_value()) {
        server.stop();
        std::cerr << "failed to start deadline runtime: "
                  << runtime_started.error().message() << "\n";
        return 1;
    }

    TestState state;
    auto root = runtime.spawn_io(run_deadline_cancel_client(port, &state));
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
