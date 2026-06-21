#include "result_writer.h"

#include <galay/cpp/galay-rpc/kernel/rpc_channel.h>
#include <galay/cpp/galay-rpc/kernel/rpc_server.h>
#include <galay/cpp/galay-rpc/kernel/rpc_service.h>
#include <galay/cpp/galay-kernel/async/tcp_socket.h>
#include <galay/cpp/galay-kernel/common/sleep.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

using namespace galay::kernel;
using namespace galay::rpc;
using namespace galay::async;
using namespace std::chrono_literals;

namespace {

uint16_t testPort()
{
    const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
    return static_cast<uint16_t>(28000 + (static_cast<uint64_t>(ticks) % 16000));
}

std::string payloadString(const RpcResponse& response)
{
    return std::string(response.payload().data(), response.payload().size());
}

std::string withMessage(std::string prefix, std::string_view message)
{
    prefix.append(message.data(), message.size());
    return prefix;
}

class ChannelTestService : public RpcService {
public:
    ChannelTestService() : RpcService("ChannelTest")
    {
        registerMethod("echo", &ChannelTestService::echo);
        registerMethod("hold", &ChannelTestService::hold);
    }

    Task<void> echo(RpcContext& ctx)
    {
        ctx.setPayload(ctx.request().payloadView());
        co_return;
    }

    Task<void> hold(RpcContext& ctx)
    {
        co_await galay::kernel::sleep(200ms);
        ctx.setPayload(ctx.request().payloadView());
        co_return;
    }
};

struct ScenarioResult {
    bool ok = false;
    bool connected = false;
    std::string failure;
};

void fail(ScenarioResult* result, std::string message)
{
    result->ok = false;
    if (result->failure.empty()) {
        result->failure = std::move(message);
    }
}

Task<std::expected<void, RpcError>> connectWithRetry(RpcChannel* channel)
{
    std::expected<void, RpcError> last_error = std::unexpected(
        RpcError(RpcErrorCode::CONNECTION_CLOSED, "connect not attempted"));
    for (int attempt = 0; attempt < 50; ++attempt) {
        auto connected = co_await channel->connect();
        if (connected.has_value() && connected.value().has_value()) {
            co_return std::expected<void, RpcError>{};
        }
        if (connected.has_value()) {
            last_error = std::unexpected(connected.value().error());
        } else {
            last_error = std::unexpected(RpcError(RpcErrorCode::INTERNAL_ERROR,
                                                   std::string(connected.error().message())));
        }
        co_await galay::kernel::sleep(10ms);
    }
    co_return last_error;
}

Task<void> oneCall(RpcChannel* channel,
                   int index,
                   std::shared_ptr<std::vector<std::string>> responses,
                   std::shared_ptr<std::atomic<int>> errors,
                   std::shared_ptr<std::atomic<int>> completed,
                   std::shared_ptr<std::atomic<int>> first_error_code)
{
    const std::string payload = "payload-" + std::to_string(index);
    auto task_result = co_await channel->call("ChannelTest", "echo", payload);
    if (!task_result.has_value()) {
        int expected = 0;
        first_error_code->compare_exchange_strong(expected, -1, std::memory_order_acq_rel);
        ++(*errors);
        ++(*completed);
        co_return;
    }
    RpcCallResult call = std::move(task_result.value());
    if (!call.has_value()) {
        int expected = 0;
        first_error_code->compare_exchange_strong(expected,
                                                  static_cast<int>(call.error().code()),
                                                  std::memory_order_acq_rel);
        ++(*errors);
        ++(*completed);
        co_return;
    }
    if (!call.value().has_value()) {
        int expected = 0;
        first_error_code->compare_exchange_strong(expected, -2, std::memory_order_acq_rel);
        ++(*errors);
        ++(*completed);
        co_return;
    }

    const RpcResponse& response = call.value().value();
    if (response.errorCode() != RpcErrorCode::OK || payloadString(response) != payload) {
        int expected = 0;
        first_error_code->compare_exchange_strong(expected, -3, std::memory_order_acq_rel);
        ++(*errors);
        ++(*completed);
        co_return;
    }
    (*responses)[static_cast<size_t>(index)] = payloadString(response);
    ++(*completed);
}

Task<void> runConcurrentUnary(uint16_t port, ScenarioResult* result)
{
    RpcChannelConfig config;
    config.host = "127.0.0.1";
    config.port = port;
    config.ring_buffer_size = 128 * 1024;
    config.max_outstanding_requests = 128;
    config.connect_timeout = 500ms;

    RpcChannel channel(config);
    auto connected = co_await connectWithRetry(&channel);
    if (!connected.has_value()) {
        fail(result, withMessage("connect task failed: ", connected.error().message()));
        co_return;
    }
    result->connected = true;
    if (channel.state() != RpcChannelState::Ready) {
        fail(result, "channel did not enter Ready state");
        co_return;
    }

    auto responses = std::make_shared<std::vector<std::string>>(100);
    auto errors = std::make_shared<std::atomic<int>>(0);
    auto completed = std::make_shared<std::atomic<int>>(0);
    auto first_error_code = std::make_shared<std::atomic<int>>(0);
    auto runtime = RuntimeHandle::current();
    if (!runtime.has_value()) {
        fail(result, "missing runtime handle");
        co_return;
    }

    for (int i = 0; i < 100; ++i) {
        auto scheduled = runtime->spawn(oneCall(&channel, i, responses, errors, completed, first_error_code));
        if (!scheduled.has_value()) {
            fail(result, "failed to schedule call task");
            co_return;
        }
    }

    for (int spin = 0; spin < 200; ++spin) {
        if (completed->load(std::memory_order_acquire) == 100 &&
            errors->load(std::memory_order_acquire) != 0) {
            fail(result, "concurrent call failed, first error code " +
                         std::to_string(first_error_code->load(std::memory_order_acquire)));
            (void)co_await channel.shutdown();
            co_return;
        }
        bool all_done = true;
        for (int i = 0; i < 100; ++i) {
            if ((*responses)[static_cast<size_t>(i)].empty()) {
                all_done = false;
                break;
            }
        }
        if (all_done) {
            const auto status = channel.status();
            if (status.connections_established != 1) {
                fail(result, "expected one persistent connection, observed " +
                                 std::to_string(status.connections_established));
                (void)co_await channel.shutdown();
                co_return;
            }
            result->ok = true;
            (void)co_await channel.shutdown();
            co_return;
        }
        co_await galay::kernel::sleep(5ms);
    }

    fail(result, errors->load(std::memory_order_acquire) == 0
                     ? "concurrent calls did not complete"
                     : "concurrent call failed, first error code " +
                           std::to_string(first_error_code->load(std::memory_order_acquire)));
    (void)co_await channel.shutdown();
}

Task<void> runOutstandingLimit(uint16_t port, ScenarioResult* result)
{
    RpcChannelConfig config;
    config.host = "127.0.0.1";
    config.port = port;
    config.ring_buffer_size = 64 * 1024;
    config.max_outstanding_requests = 0;
    config.connect_timeout = 500ms;

    RpcChannel channel(config);
    auto connected = co_await connectWithRetry(&channel);
    if (!connected.has_value()) {
        fail(result, withMessage("connect task failed: ", connected.error().message()));
        co_return;
    }
    result->connected = true;

    auto task_result = co_await channel.call("ChannelTest", "echo", std::string("over-limit"));
    if (!task_result.has_value()) {
        fail(result, withMessage("call task failed: ", task_result.error().message()));
        (void)co_await channel.shutdown();
        co_return;
    }

    RpcCallResult call = std::move(task_result.value());
    if (call.has_value()) {
        fail(result, "max outstanding call unexpectedly succeeded");
        (void)co_await channel.shutdown();
        co_return;
    }
    if (call.error().code() != RpcErrorCode::RESOURCE_EXHAUSTED) {
        fail(result, "wrong max outstanding error code");
        (void)co_await channel.shutdown();
        co_return;
    }

    result->ok = true;
    (void)co_await channel.shutdown();
}

Task<void> runCloseRejectsNewCalls(uint16_t port, ScenarioResult* result)
{
    RpcChannelConfig config;
    config.host = "127.0.0.1";
    config.port = port;
    config.ring_buffer_size = 64 * 1024;
    config.max_outstanding_requests = 8;
    config.connect_timeout = 500ms;

    RpcChannel channel(config);
    auto connected = co_await connectWithRetry(&channel);
    if (!connected.has_value()) {
        fail(result, withMessage("connect task failed: ", connected.error().message()));
        co_return;
    }
    result->connected = true;

    (void)co_await channel.close();
    (void)co_await channel.close();

    auto task_result = co_await channel.call("ChannelTest", "echo", std::string("closed"));
    if (!task_result.has_value()) {
        fail(result, withMessage("call task failed: ", task_result.error().message()));
        co_return;
    }

    RpcCallResult call = std::move(task_result.value());
    if (call.has_value()) {
        fail(result, "call after close unexpectedly succeeded");
        co_return;
    }
    if (call.error().code() != RpcErrorCode::CONNECTION_CLOSED) {
        fail(result, "wrong close-after-call error code");
        co_return;
    }

    result->ok = true;
}

Task<void> pendingHoldCall(RpcChannel* channel,
                           std::shared_ptr<std::atomic<int>> completed,
                           std::shared_ptr<std::atomic<int>> closed_errors)
{
    auto task_result = co_await channel->call("ChannelTest", "hold", std::string("pending"));
    completed->fetch_add(1, std::memory_order_relaxed);
    if (task_result.has_value() && !task_result.value().has_value() &&
        task_result.value().error().code() == RpcErrorCode::CONNECTION_CLOSED) {
        closed_errors->fetch_add(1, std::memory_order_relaxed);
    }
    co_return;
}

Task<void> rawServerCloseOnce(uint16_t port)
{
    TcpSocket listener(IPType::IPV4);
    (void)listener.option().handleReuseAddr();
    (void)listener.option().handleNonBlock();
    auto bind_result = listener.bind(Host(IPType::IPV4, "127.0.0.1", port));
    if (!bind_result.has_value()) {
        co_return;
    }
    auto listen_result = listener.listen(8);
    if (!listen_result.has_value()) {
        (void)co_await listener.close();
        co_return;
    }

    Host client_host;
    auto accepted = co_await listener.accept(&client_host);
    if (accepted.has_value()) {
        TcpSocket client(accepted.value());
        co_await galay::kernel::sleep(30ms);
        (void)co_await client.close();
    }
    (void)co_await listener.close();
}

Task<void> runConnectionCloseWakesPending(uint16_t port, ScenarioResult* result)
{
    RpcChannelConfig config;
    config.host = "127.0.0.1";
    config.port = port;
    config.ring_buffer_size = 64 * 1024;
    config.max_outstanding_requests = 16;
    config.connect_timeout = 500ms;

    RpcChannel channel(config);
    auto connected = co_await connectWithRetry(&channel);
    if (!connected.has_value()) {
        fail(result, withMessage("connect task failed: ", connected.error().message()));
        co_return;
    }
    result->connected = true;

    auto runtime = RuntimeHandle::current();
    if (!runtime.has_value()) {
        fail(result, "missing runtime handle");
        co_return;
    }

    auto completed = std::make_shared<std::atomic<int>>(0);
    auto closed_errors = std::make_shared<std::atomic<int>>(0);

    for (int i = 0; i < 8; ++i) {
        auto scheduled = runtime->spawn(pendingHoldCall(&channel, completed, closed_errors));
        if (!scheduled.has_value()) {
            fail(result, "failed to schedule pending call task");
            co_return;
        }
    }

    co_await galay::kernel::sleep(20ms);
    (void)co_await channel.close();

    for (int spin = 0; spin < 200; ++spin) {
        if (completed->load(std::memory_order_acquire) == 8) {
            if (closed_errors->load(std::memory_order_acquire) != 8) {
                fail(result, "pending calls completed without CONNECTION_CLOSED");
                (void)co_await channel.shutdown();
                co_return;
            }
            result->ok = true;
            (void)co_await channel.shutdown();
            co_return;
        }
        co_await galay::kernel::sleep(5ms);
    }

    fail(result, "pending calls were not woken after channel close");
    (void)co_await channel.shutdown();
}

Task<void> runRemoteCloseWakesPending(uint16_t port, ScenarioResult* result)
{
    auto runtime = RuntimeHandle::current();
    if (!runtime.has_value()) {
        fail(result, "missing runtime handle");
        co_return;
    }
    auto server_task = runtime->spawn(rawServerCloseOnce(port));
    if (!server_task.has_value()) {
        fail(result, "failed to schedule raw close server");
        co_return;
    }

    RpcChannelConfig config;
    config.host = "127.0.0.1";
    config.port = port;
    config.ring_buffer_size = 64 * 1024;
    config.max_outstanding_requests = 16;
    config.connect_timeout = 500ms;

    RpcChannel channel(config);
    auto connected = co_await connectWithRetry(&channel);
    if (!connected.has_value()) {
        fail(result, withMessage("connect task failed: ", connected.error().message()));
        co_return;
    }
    result->connected = true;

    auto completed = std::make_shared<std::atomic<int>>(0);
    auto closed_errors = std::make_shared<std::atomic<int>>(0);
    for (int i = 0; i < 8; ++i) {
        auto scheduled = runtime->spawn(pendingHoldCall(&channel, completed, closed_errors));
        if (!scheduled.has_value()) {
            fail(result, "failed to schedule pending call task");
            co_return;
        }
    }

    for (int spin = 0; spin < 200; ++spin) {
        if (completed->load(std::memory_order_acquire) == 8) {
            if (closed_errors->load(std::memory_order_acquire) != 8) {
                fail(result, "remote close completed pending calls without CONNECTION_CLOSED");
                (void)co_await channel.shutdown();
                co_return;
            }
            result->ok = true;
            (void)co_await channel.shutdown();
            co_return;
        }
        co_await galay::kernel::sleep(5ms);
    }

    fail(result, "pending calls were not woken after remote server close");
    (void)co_await channel.shutdown();
}

} // namespace

int main()
{
    test::TestResultWriter writer("t9_channel_unary_concurrency.result");
    std::cout << "Running RPC channel concurrency tests...\n";

    const uint16_t port = testPort();
    auto server = RpcServerBuilder()
        .host("127.0.0.1")
        .port(port)
        .ioSchedulerCount(1)
        .computeSchedulerCount(0)
        .ringBufferSize(128 * 1024)
        .build();
    server.registerService(std::make_shared<ChannelTestService>());
    server.start();

    ScenarioResult concurrent;
    Runtime concurrent_runtime = RuntimeBuilder().ioSchedulerCount(1).computeSchedulerCount(0).build();
    auto concurrent_result = concurrent_runtime.blockOn(runConcurrentUnary(port, &concurrent));
    concurrent_runtime.stop();
    if (!concurrent_result.has_value()) {
        concurrent.failure = std::string(concurrent_result.error().message());
    }
    writer.writeTestCase("RpcChannel dispatches 100 concurrent unary responses by request id",
                         concurrent.ok,
                         concurrent.failure);

    ScenarioResult limit;
    Runtime limit_runtime = RuntimeBuilder().ioSchedulerCount(1).computeSchedulerCount(0).build();
    auto limit_result = limit_runtime.blockOn(runOutstandingLimit(port, &limit));
    limit_runtime.stop();
    if (!limit_result.has_value()) {
        limit.failure = std::string(limit_result.error().message());
    }
    writer.writeTestCase("RpcChannel returns RESOURCE_EXHAUSTED over max outstanding",
                         limit.ok,
                         limit.failure);

    ScenarioResult closed;
    Runtime closed_runtime = RuntimeBuilder().ioSchedulerCount(1).computeSchedulerCount(0).build();
    auto closed_result = closed_runtime.blockOn(runCloseRejectsNewCalls(port, &closed));
    closed_runtime.stop();
    if (!closed_result.has_value()) {
        closed.failure = std::string(closed_result.error().message());
    }
    writer.writeTestCase("RpcChannel close is idempotent and rejects new calls",
                         closed.ok,
                         closed.failure);

    server.stop();

    const uint16_t close_port = testPort();
    auto closing_server = RpcServerBuilder()
        .host("127.0.0.1")
        .port(close_port)
        .ioSchedulerCount(1)
        .computeSchedulerCount(0)
        .ringBufferSize(128 * 1024)
        .build();
    closing_server.registerService(std::make_shared<ChannelTestService>());
    closing_server.start();

    ScenarioResult wake_pending;
    Runtime wake_runtime = RuntimeBuilder().ioSchedulerCount(1).computeSchedulerCount(0).build();
    auto wake_result = wake_runtime.blockOn(runConnectionCloseWakesPending(close_port,
                                                                           &wake_pending));
    wake_runtime.stop();
    if (!wake_result.has_value()) {
        wake_pending.failure = std::string(wake_result.error().message());
    }
    closing_server.stop();
    writer.writeTestCase("RpcChannel wakes pending calls when the connection closes",
                         wake_pending.ok,
                         wake_pending.failure);

    const uint16_t remote_close_port = testPort();
    ScenarioResult remote_close;
    Runtime remote_close_runtime = RuntimeBuilder().ioSchedulerCount(1).computeSchedulerCount(0).build();
    auto remote_close_result = remote_close_runtime.blockOn(runRemoteCloseWakesPending(remote_close_port,
                                                                                      &remote_close));
    remote_close_runtime.stop();
    if (!remote_close_result.has_value()) {
        remote_close.failure = std::string(remote_close_result.error().message());
    }
    writer.writeTestCase("RpcChannel wakes pending calls when the remote server closes the socket",
                         remote_close.ok,
                         remote_close.failure);

    writer.writeSummary();

    std::cout << "Tests completed. Passed: " << writer.passed()
              << ", Failed: " << writer.failed() << "\n";
    return writer.failed() > 0 ? 1 : 0;
}
