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
        register_stream_method("echo", &StreamLoopbackService::echo);
    }

    Task<void> echo(RpcStream& stream)
    {
        while (true) {
            StreamMessage msg;
            auto recv_result = co_await stream.read(msg);
            if (!recv_result.has_value()) {
                co_return;
            }

            if (msg.message_type() == RpcMessageType::STREAM_DATA) {
                auto send_result = co_await stream.send_data(msg.payload_view());
                if (!send_result.has_value()) {
                    co_return;
                }
                continue;
            }

            if (msg.message_type() == RpcMessageType::STREAM_END) {
                (void)co_await stream.send_end();
                co_return;
            }

            if (msg.message_type() == RpcMessageType::STREAM_CANCEL) {
                co_return;
            }

            (void)co_await stream.send_cancel();
            co_return;
        }
    }
};

struct StreamResult {
    bool done = false;
    bool ok = true;
    std::string error;
};

uint16_t loopback_port()
{
    return static_cast<uint16_t>(23000 + (::getpid() % 20000));
}

void fail(StreamResult& state, std::string message)
{
    state.ok = false;
    state.error = std::move(message);
}

Task<bool> connect_client(RpcClient& client, uint16_t port)
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

Task<bool> expect_cancel(uint16_t port,
                        uint32_t stream_id,
                        const std::string& service,
                        const std::string& method)
{
    RpcClient client;
    if (!co_await connect_client(client, port)) {
        co_return false;
    }

    auto stream_result = client.create_stream(stream_id, service, method);
    if (!stream_result.has_value()) {
        co_await client.close();
        co_return false;
    }
    auto stream = std::move(stream_result.value());

    auto send_result = co_await stream.send_init();
    if (!send_result.has_value()) {
        co_await client.close();
        co_return false;
    }

    StreamMessage msg;
    auto recv_result = co_await stream.read(msg);
    const bool ok = recv_result.has_value() &&
                    msg.message_type() == RpcMessageType::STREAM_CANCEL &&
                    msg.stream_id() == stream_id;
    co_await client.close();
    co_return ok;
}

Task<void> run_stream_client(uint16_t port, StreamResult* state)
{
    RpcClient client;
    if (!co_await connect_client(client, port)) {
        fail(*state, "stream client connect retry exhausted");
        state->done = true;
        co_return;
    }

    auto stream_result = client.create_stream(1, "StreamLoopbackService", "echo");
    if (!stream_result.has_value()) {
        fail(*state, "create stream failed");
        state->done = true;
        co_return;
    }
    auto stream = std::move(stream_result.value());

    auto send_result = co_await stream.send_init();
    if (!send_result.has_value()) {
        fail(*state, "send init failed");
        co_await client.close();
        state->done = true;
        co_return;
    }

    StreamMessage init_ack;
    auto recv_result = co_await stream.read(init_ack);
    if (!recv_result.has_value() ||
        init_ack.message_type() != RpcMessageType::STREAM_INIT_ACK ||
        init_ack.stream_id() != 1) {
        fail(*state, "init ack failed");
        co_await client.close();
        state->done = true;
        co_return;
    }

    for (const std::string payload : {"alpha", "beta", "gamma"}) {
        send_result = co_await stream.send_data(payload);
        if (!send_result.has_value()) {
            fail(*state, "send data failed");
            co_await client.close();
            state->done = true;
            co_return;
        }

        StreamMessage echo;
        recv_result = co_await stream.read(echo);
        if (!recv_result.has_value() ||
            echo.message_type() != RpcMessageType::STREAM_DATA ||
            echo.payload_str() != payload) {
            fail(*state, "echo frame mismatch");
            co_await client.close();
            state->done = true;
            co_return;
        }
    }

    send_result = co_await stream.send_end();
    if (!send_result.has_value()) {
        fail(*state, "send end failed");
        co_await client.close();
        state->done = true;
        co_return;
    }

    StreamMessage end_msg;
    recv_result = co_await stream.read(end_msg);
    if (!recv_result.has_value() || end_msg.message_type() != RpcMessageType::STREAM_END) {
        fail(*state, "server end frame missing");
        co_await client.close();
        state->done = true;
        co_return;
    }

    co_await client.close();

    if (!co_await expect_cancel(port, 2, "MissingService", "echo")) {
        fail(*state, "missing stream service did not return cancel");
        state->done = true;
        co_return;
    }

    if (!co_await expect_cancel(port, 3, "StreamLoopbackService", "missing")) {
        fail(*state, "missing stream method did not return cancel");
        state->done = true;
        co_return;
    }

    RpcClient invalid_client;
    if (!co_await connect_client(invalid_client, port)) {
        fail(*state, "invalid-frame client connect failed");
        state->done = true;
        co_return;
    }
    auto invalid_stream_result = invalid_client.create_stream(4);
    if (!invalid_stream_result.has_value()) {
        fail(*state, "invalid stream create failed");
        co_await invalid_client.close();
        state->done = true;
        co_return;
    }
    auto invalid_stream = std::move(invalid_stream_result.value());
    send_result = co_await invalid_stream.send_data("not-init");
    StreamMessage cancel_msg;
    recv_result = co_await invalid_stream.read(cancel_msg);
    if (!send_result.has_value() ||
        !recv_result.has_value() ||
        cancel_msg.message_type() != RpcMessageType::STREAM_CANCEL ||
        cancel_msg.stream_id() != 4) {
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
    const uint16_t port = loopback_port();

    auto server = RpcStreamServerBuilder()
        .host("127.0.0.1")
        .port(port)
        .io_scheduler_count(1)
        .parallel_scheduler_count(0)
        .build();
    StreamLoopbackService service;
    auto registered = server.register_service(service);
    if (!registered.has_value()) {
        std::cerr << "failed to register stream loopback service: "
                  << registered.error().message() << "\n";
        return 1;
    }
    auto server_started = server.start();
    if (!server_started.has_value()) {
        std::cerr << "failed to start stream loopback server: "
                  << server_started.error().message() << "\n";
        return 1;
    }

    Runtime runtime = RuntimeBuilder().io_scheduler_count(1).parallel_scheduler_count(0).build();
    auto runtime_started = runtime.start();
    if (!runtime_started.has_value()) {
        server.stop();
        std::cerr << "failed to start stream loopback runtime: "
                  << runtime_started.error().message() << "\n";
        return 1;
    }

    StreamResult state;
    if (!schedule_task(runtime.get_next_io_scheduler(), run_stream_client(port, &state))) {
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
