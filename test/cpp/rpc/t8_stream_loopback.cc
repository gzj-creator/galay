/**
 * @file t8_stream_loopback.cc
 * @brief RPC stream loopback 测试
 */

#include <galay/cpp/galay-rpc/kernel/rpc_client.h>
#include <galay/cpp/galay-rpc/kernel/rpc_service.h>
#include <galay/cpp/galay-rpc/kernel/rpc_stream.h>
#include <galay/cpp/galay-rpc/kernel/streamsvc.h>
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

class StreamLoopbackService final : public RpcService {
public:
    StreamLoopbackService()
        : RpcService("StreamLoopbackService")
    {
        registerStreamMethod("echo", &StreamLoopbackService::echo);
    }

    Task<void> echo(RpcStream& stream)
    {
        while (true) {
            StreamMessage msg;
            auto recv_result = co_await stream.read(msg);
            if (!recv_result.has_value()) {
                co_return;
            }

            if (msg.messageType() == RpcMessageType::STREAM_DATA) {
                auto send_result = co_await stream.sendData(msg.payloadView());
                if (!send_result.has_value()) {
                    co_return;
                }
                continue;
            }

            if (msg.messageType() == RpcMessageType::STREAM_END) {
                (void)co_await stream.sendEnd();
                co_return;
            }

            if (msg.messageType() == RpcMessageType::STREAM_CANCEL) {
                co_return;
            }

            (void)co_await stream.sendCancel();
            co_return;
        }
    }
};

struct StreamResult {
    bool done = false;
    bool ok = true;
    std::string error;
};

uint16_t loopbackPort()
{
    return static_cast<uint16_t>(23000 + (::getpid() % 20000));
}

void fail(StreamResult& state, std::string message)
{
    state.ok = false;
    state.error = std::move(message);
}

Task<bool> connectClient(RpcClient& client, uint16_t port)
{
    for (int attempt = 0; attempt < 100; ++attempt) {
        auto connect_result = co_await client.connect("127.0.0.1", port);
        if (connect_result.has_value()) {
            co_return true;
        }
        co_await sleep(std::chrono::milliseconds(10));
    }
    co_return false;
}

Task<bool> expectCancel(uint16_t port,
                        uint32_t stream_id,
                        const std::string& service,
                        const std::string& method)
{
    RpcClient client;
    if (!co_await connectClient(client, port)) {
        co_return false;
    }

    auto stream_result = client.createStream(stream_id, service, method);
    if (!stream_result.has_value()) {
        co_await client.close();
        co_return false;
    }
    auto stream = stream_result.value();

    auto send_result = co_await stream.sendInit();
    if (!send_result.has_value()) {
        co_await client.close();
        co_return false;
    }

    StreamMessage msg;
    auto recv_result = co_await stream.read(msg);
    const bool ok = recv_result.has_value() &&
                    msg.messageType() == RpcMessageType::STREAM_CANCEL &&
                    msg.streamId() == stream_id;
    co_await client.close();
    co_return ok;
}

Task<void> runStreamClient(uint16_t port, StreamResult* state)
{
    RpcClient client;
    if (!co_await connectClient(client, port)) {
        fail(*state, "stream client connect retry exhausted");
        state->done = true;
        co_return;
    }

    auto stream_result = client.createStream(1, "StreamLoopbackService", "echo");
    if (!stream_result.has_value()) {
        fail(*state, "create stream failed");
        state->done = true;
        co_return;
    }
    auto stream = stream_result.value();

    auto send_result = co_await stream.sendInit();
    if (!send_result.has_value()) {
        fail(*state, "send init failed");
        co_await client.close();
        state->done = true;
        co_return;
    }

    StreamMessage init_ack;
    auto recv_result = co_await stream.read(init_ack);
    if (!recv_result.has_value() ||
        init_ack.messageType() != RpcMessageType::STREAM_INIT_ACK ||
        init_ack.streamId() != 1) {
        fail(*state, "init ack failed");
        co_await client.close();
        state->done = true;
        co_return;
    }

    for (const std::string payload : {"alpha", "beta", "gamma"}) {
        send_result = co_await stream.sendData(payload);
        if (!send_result.has_value()) {
            fail(*state, "send data failed");
            co_await client.close();
            state->done = true;
            co_return;
        }

        StreamMessage echo;
        recv_result = co_await stream.read(echo);
        if (!recv_result.has_value() ||
            echo.messageType() != RpcMessageType::STREAM_DATA ||
            echo.payloadStr() != payload) {
            fail(*state, "echo frame mismatch");
            co_await client.close();
            state->done = true;
            co_return;
        }
    }

    send_result = co_await stream.sendEnd();
    if (!send_result.has_value()) {
        fail(*state, "send end failed");
        co_await client.close();
        state->done = true;
        co_return;
    }

    StreamMessage end_msg;
    recv_result = co_await stream.read(end_msg);
    if (!recv_result.has_value() || end_msg.messageType() != RpcMessageType::STREAM_END) {
        fail(*state, "server end frame missing");
        co_await client.close();
        state->done = true;
        co_return;
    }

    co_await client.close();

    if (!co_await expectCancel(port, 2, "MissingService", "echo")) {
        fail(*state, "missing stream service did not return cancel");
        state->done = true;
        co_return;
    }

    if (!co_await expectCancel(port, 3, "StreamLoopbackService", "missing")) {
        fail(*state, "missing stream method did not return cancel");
        state->done = true;
        co_return;
    }

    RpcClient invalid_client;
    if (!co_await connectClient(invalid_client, port)) {
        fail(*state, "invalid-frame client connect failed");
        state->done = true;
        co_return;
    }
    auto invalid_stream_result = invalid_client.createStream(4);
    if (!invalid_stream_result.has_value()) {
        fail(*state, "invalid stream create failed");
        co_await invalid_client.close();
        state->done = true;
        co_return;
    }
    auto invalid_stream = invalid_stream_result.value();
    send_result = co_await invalid_stream.sendData("not-init");
    StreamMessage cancel_msg;
    recv_result = co_await invalid_stream.read(cancel_msg);
    if (!send_result.has_value() ||
        !recv_result.has_value() ||
        cancel_msg.messageType() != RpcMessageType::STREAM_CANCEL ||
        cancel_msg.streamId() != 4) {
        fail(*state, "invalid first frame did not return cancel");
        co_await invalid_client.close();
        state->done = true;
        co_return;
    }
    co_await invalid_client.close();

    state->done = true;
    co_return;
}

} // namespace

int main()
{
    const uint16_t port = loopbackPort();

    auto server = RpcStreamServerBuilder()
        .host("127.0.0.1")
        .port(port)
        .ioSchedulerCount(1)
        .computeSchedulerCount(0)
        .build();
    server.registerService(std::make_shared<StreamLoopbackService>());
    server.start();

    Runtime runtime = RuntimeBuilder().ioSchedulerCount(1).computeSchedulerCount(0).build();
    runtime.start();

    StreamResult state;
    if (!scheduleTask(runtime.getNextIOScheduler(), runStreamClient(port, &state))) {
        runtime.stop();
        server.stop();
        std::cerr << "failed to schedule stream client\n";
        return 1;
    }

    for (int i = 0; i < 300 && !state.done; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    runtime.stop();
    server.stop();

    if (!state.done) {
        std::cerr << "stream loopback timed out\n";
        return 1;
    }
    if (!state.ok) {
        std::cerr << state.error << "\n";
        return 1;
    }

    std::cout << "RPC stream loopback PASS\n";
    return 0;
}
