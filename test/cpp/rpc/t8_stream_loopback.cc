#include "result_writer.h"

#include <galay/cpp/galay-rpc/kernel/rpc_client.h>
#include <galay/cpp/galay-rpc/kernel/rpc_service.h>
#include <galay/cpp/galay-rpc/kernel/rpc_stream.h>
#include <galay/cpp/galay-rpc/kernel/streamsvc.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

using namespace galay::kernel;
using namespace galay::rpc;
using namespace std::chrono_literals;

namespace {

uint16_t testPort()
{
    const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
    return static_cast<uint16_t>(28000 + (static_cast<uint64_t>(ticks) % 16000));
}

struct ClientResult {
    bool connected = false;
    bool ok = false;
    std::string failure;
    std::string summary;
};

void fail(ClientResult* result, std::string message)
{
    result->ok = false;
    if (result->failure.empty()) {
        result->failure = std::move(message);
    }
}

class StreamLoopbackService : public RpcService {
public:
    StreamLoopbackService() : RpcService("StreamLoopbackService")
    {
        registerStreamMethod("echo", &StreamLoopbackService::echo);
    }

    Task<void> echo(RpcStream& stream)
    {
        uint64_t frames = 0;
        uint64_t bytes = 0;

        while (true) {
            StreamMessage msg;
            auto recv_result = co_await stream.read(msg);
            if (!recv_result.has_value()) {
                co_return;
            }

            if (msg.messageType() == RpcMessageType::STREAM_DATA) {
                ++frames;
                bytes += msg.payloadSize();
                auto send_result = co_await stream.sendData(msg.payloadView());
                if (!send_result.has_value()) {
                    co_return;
                }
                continue;
            }

            if (msg.messageType() == RpcMessageType::STREAM_END) {
                const std::string summary =
                    "frames=" + std::to_string(frames) +
                    ",bytes=" + std::to_string(bytes);
                auto send_result = co_await stream.sendData(summary);
                if (!send_result.has_value()) {
                    co_return;
                }
                (void)co_await stream.sendEnd();
                co_return;
            }

            (void)co_await stream.sendCancel();
            co_return;
        }
    }
};

bool expectMessage(ClientResult* result,
                   const StreamMessage& message,
                   RpcMessageType type,
                   uint32_t stream_id,
                   std::string_view payload = {})
{
    if (message.messageType() != type || message.streamId() != stream_id) {
        fail(result,
             "unexpected stream frame type=" + std::to_string(static_cast<int>(message.messageType())) +
             " stream_id=" + std::to_string(message.streamId()));
        return false;
    }

    if (!payload.empty() && !message.payloadEquals(payload)) {
        fail(result, "unexpected stream payload: " + message.payloadStr());
        return false;
    }
    return true;
}

Task<void> runHappyClient(uint16_t port, ClientResult* result)
{
    RpcClient client = RpcClientBuilder().ringBufferSize(64 * 1024).build();
    auto connected = co_await client.connect("127.0.0.1", port).timeout(200ms);
    if (!connected.has_value()) {
        result->connected = false;
        fail(result, "connect failed: " + connected.error().message());
        co_return;
    }
    result->connected = true;

    auto stream_result = client.createStream(77, "StreamLoopbackService", "echo");
    if (!stream_result.has_value()) {
        fail(result, stream_result.error().message());
        (void)co_await client.close();
        co_return;
    }
    auto stream = std::move(stream_result.value());

    auto send_result = co_await stream.sendInit().timeout(200ms);
    if (!send_result.has_value()) {
        fail(result, send_result.error().message());
        (void)co_await client.close();
        co_return;
    }

    StreamMessage ack;
    auto recv_result = co_await stream.read(ack).timeout(200ms);
    if (!recv_result.has_value() ||
        !expectMessage(result, ack, RpcMessageType::STREAM_INIT_ACK, 77)) {
        fail(result, recv_result.has_value() ? result->failure : recv_result.error().message());
        (void)co_await client.close();
        co_return;
    }

    const std::string first = "alpha";
    const std::string second = "beta";
    (void)co_await stream.sendData(first).timeout(200ms);
    StreamMessage first_echo;
    recv_result = co_await stream.read(first_echo).timeout(200ms);
    if (!recv_result.has_value() ||
        !expectMessage(result, first_echo, RpcMessageType::STREAM_DATA, 77, first)) {
        fail(result, recv_result.has_value() ? result->failure : recv_result.error().message());
        (void)co_await client.close();
        co_return;
    }

    (void)co_await stream.sendData(second).timeout(200ms);
    StreamMessage second_echo;
    recv_result = co_await stream.read(second_echo).timeout(200ms);
    if (!recv_result.has_value() ||
        !expectMessage(result, second_echo, RpcMessageType::STREAM_DATA, 77, second)) {
        fail(result, recv_result.has_value() ? result->failure : recv_result.error().message());
        (void)co_await client.close();
        co_return;
    }

    (void)co_await stream.sendEnd().timeout(200ms);
    StreamMessage summary;
    recv_result = co_await stream.read(summary).timeout(200ms);
    if (!recv_result.has_value() ||
        !expectMessage(result, summary, RpcMessageType::STREAM_DATA, 77)) {
        fail(result, recv_result.has_value() ? result->failure : recv_result.error().message());
        (void)co_await client.close();
        co_return;
    }
    result->summary = summary.payloadStr();

    StreamMessage end;
    recv_result = co_await stream.read(end).timeout(200ms);
    if (!recv_result.has_value() ||
        !expectMessage(result, end, RpcMessageType::STREAM_END, 77)) {
        fail(result, recv_result.has_value() ? result->failure : recv_result.error().message());
        (void)co_await client.close();
        co_return;
    }

    result->ok = result->summary == "frames=2,bytes=9";
    if (!result->ok) {
        fail(result, "unexpected summary: " + result->summary);
    }
    (void)co_await client.close();
}

Task<void> runMissingRouteClient(uint16_t port, ClientResult* result)
{
    RpcClient client = RpcClientBuilder().ringBufferSize(64 * 1024).build();
    auto connected = co_await client.connect("127.0.0.1", port).timeout(200ms);
    if (!connected.has_value()) {
        fail(result, connected.error().message());
        co_return;
    }
    result->connected = true;

    auto stream_result = client.createStream(78, "StreamLoopbackService", "missing");
    if (!stream_result.has_value()) {
        fail(result, stream_result.error().message());
        (void)co_await client.close();
        co_return;
    }
    auto stream = std::move(stream_result.value());
    (void)co_await stream.sendInit().timeout(200ms);

    StreamMessage cancel;
    auto recv_result = co_await stream.read(cancel).timeout(200ms);
    result->ok = recv_result.has_value() &&
                 cancel.messageType() == RpcMessageType::STREAM_CANCEL &&
                 cancel.streamId() == 78;
    if (!result->ok) {
        fail(result, recv_result.has_value() ? "missing route did not cancel" : recv_result.error().message());
    }
    (void)co_await client.close();
}

bool runClientWithRetry(uint16_t port, ClientResult* result, bool missing_route)
{
    for (int attempt = 0; attempt < 50; ++attempt) {
        ClientResult attempt_result;
        Runtime runtime = RuntimeBuilder().ioSchedulerCount(1).computeSchedulerCount(0).build();
        auto run_result = missing_route
            ? runtime.blockOn(runMissingRouteClient(port, &attempt_result))
            : runtime.blockOn(runHappyClient(port, &attempt_result));
        runtime.stop();

        if (!run_result.has_value()) {
            attempt_result.failure = run_result.error().message();
        }
        if (attempt_result.ok) {
            *result = std::move(attempt_result);
            return true;
        }
        if (attempt_result.connected) {
            *result = std::move(attempt_result);
            return false;
        }
        *result = std::move(attempt_result);
        std::this_thread::sleep_for(10ms);
    }
    return false;
}

} // namespace

int main()
{
    test::TestResultWriter writer("t8_stream_loopback.result");
    std::cout << "Running RPC stream loopback tests...\n";

    const uint16_t port = testPort();
    auto server = RpcStreamServerBuilder()
        .host("127.0.0.1")
        .port(port)
        .ioSchedulerCount(1)
        .computeSchedulerCount(0)
        .ringBufferSize(64 * 1024)
        .build();
    server.registerService(std::make_shared<StreamLoopbackService>());
    server.start();

    ClientResult happy;
    const bool happy_passed = runClientWithRetry(port, &happy, false);
    writer.writeTestCase("RpcStreamServer/RpcStream happy loopback",
                         happy_passed && happy.ok,
                         happy.failure);

    ClientResult missing_route;
    const bool missing_passed = runClientWithRetry(port, &missing_route, true);
    writer.writeTestCase("RpcStreamServer missing route returns cancel",
                         missing_passed && missing_route.ok,
                         missing_route.failure);

    server.stop();
    writer.writeSummary();

    std::cout << "Tests completed. Passed: " << writer.passed()
              << ", Failed: " << writer.failed() << "\n";
    return writer.failed() > 0 ? 1 : 0;
}
