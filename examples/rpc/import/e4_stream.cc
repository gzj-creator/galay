/**
 * @file e4_stream.cc
 * @brief 真实流式 RPC 客户端示例（C++23 import 版本）
 */

import galay.rpc;

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

using namespace galay::rpc;
using namespace galay::kernel;

namespace {
constexpr uint16_t kDefaultPort = 9100;
constexpr size_t kDefaultFrameCount = 1000;
constexpr size_t kDefaultPayloadSize = 128;

Coroutine runStreamClient(const std::string& host,
                          uint16_t port,
                          size_t frame_count,
                          size_t payload_size) {
    const size_t ring_buffer_size = std::max<size_t>(kDefaultRpcRingBufferSize,
                                                     payload_size * 4 + RPC_HEADER_SIZE * 8);
    auto client = RpcClientBuilder()
        .ringBufferSize(ring_buffer_size)
        .build();

    auto connect_result = co_await client.connect(host, port);
    if (!connect_result.has_value()) {
        std::cerr << "connect failed: " << connect_result.error().message() << "\n";
        co_return;
    }

    const uint32_t stream_id = 1;
    auto stream_result = client.createStream(stream_id, "StreamExampleService", "echo");
    if (!stream_result.has_value()) {
        std::cerr << "create stream failed: " << stream_result.error().message() << "\n";
        co_await client.close();
        co_return;
    }
    auto stream = stream_result.value();

    auto send_result = co_await stream.sendInit();
    if (!send_result.has_value()) {
        std::cerr << "send init failed: " << send_result.error().message() << "\n";
        co_await client.close();
        co_return;
    }

    StreamMessage init_ack;
    auto recv_result = co_await stream.read(init_ack);
    if (!recv_result.has_value()) {
        std::cerr << "recv init ack failed: " << recv_result.error().message() << "\n";
        co_await client.close();
        co_return;
    }

    if (init_ack.messageType() != RpcMessageType::STREAM_INIT_ACK) {
        std::cerr << "unexpected init response type: " << static_cast<int>(init_ack.messageType()) << "\n";
        co_await client.close();
        co_return;
    }

    std::string payload(payload_size, 'x');
    uint64_t total_echo_bytes = 0;

    const auto start = std::chrono::steady_clock::now();

    for (size_t i = 0; i < frame_count; ++i) {
        if (payload_size >= sizeof(uint64_t)) {
            const uint64_t frame_id = static_cast<uint64_t>(i);
            std::memcpy(payload.data(), &frame_id, sizeof(frame_id));
        }

        send_result = co_await stream.sendData(payload.data(), payload.size());
        if (!send_result.has_value()) {
            std::cerr << "send frame failed: " << send_result.error().message() << "\n";
            co_await client.close();
            co_return;
        }

        StreamMessage echo_frame;
        recv_result = co_await stream.read(echo_frame);
        if (!recv_result.has_value()) {
            std::cerr << "recv echo frame failed: " << recv_result.error().message() << "\n";
            co_await client.close();
            co_return;
        }

        if (echo_frame.messageType() != RpcMessageType::STREAM_DATA) {
            std::cerr << "unexpected frame type while streaming: "
                      << static_cast<int>(echo_frame.messageType()) << "\n";
            co_await client.close();
            co_return;
        }

        total_echo_bytes += echo_frame.payload().size();
    }

    send_result = co_await stream.sendEnd();
    if (!send_result.has_value()) {
        std::cerr << "send end failed: " << send_result.error().message() << "\n";
        co_await client.close();
        co_return;
    }

    std::string summary;
    bool got_end = false;
    while (!got_end) {
        StreamMessage msg;
        recv_result = co_await stream.read(msg);
        if (!recv_result.has_value()) {
            std::cerr << "recv tail frame failed: " << recv_result.error().message() << "\n";
            co_await client.close();
            co_return;
        }

        if (msg.messageType() == RpcMessageType::STREAM_DATA) {
            summary = msg.payloadStr();
        } else if (msg.messageType() == RpcMessageType::STREAM_END) {
            got_end = true;
        }
    }

    const auto end = std::chrono::steady_clock::now();
    const double elapsed_sec = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count() / 1000.0;
    const double frame_rate = elapsed_sec > 0 ? static_cast<double>(frame_count) / elapsed_sec : 0.0;

    std::cout << "stream_id=" << stream_id
              << ", frames=" << frame_count
              << ", payload=" << payload_size
              << " bytes\n";
    std::cout << "elapsed=" << elapsed_sec << " s"
              << ", frame_rate=" << frame_rate << " frames/s"
              << ", echoed_bytes=" << total_echo_bytes << "\n";
    std::cout << "server summary: " << summary << "\n";

    co_await client.close();
    co_return;
}
} // namespace

int main(int argc, char* argv[]) {
    std::string host = "127.0.0.1";
    uint16_t port = kDefaultPort;
    size_t frame_count = kDefaultFrameCount;
    size_t payload_size = kDefaultPayloadSize;

    if (argc > 1) {
        host = argv[1];
    }
    if (argc > 2) {
        port = static_cast<uint16_t>(std::atoi(argv[2]));
    }
    if (argc > 3) {
        frame_count = static_cast<size_t>(std::strtoull(argv[3], nullptr, 10));
    }
    if (argc > 4) {
        payload_size = static_cast<size_t>(std::strtoull(argv[4], nullptr, 10));
    }

    std::cout << "=== Stream RPC Client Example (import) ===\n";
    std::cout << "target=" << host << ":" << port
              << ", frames=" << frame_count
              << ", payload=" << payload_size << " bytes\n";

    Runtime runtime = RuntimeBuilder().ioSchedulerCount(1).computeSchedulerCount(1).build();
    runtime.start();
    (void)scheduleTask(runtime.getNextIOScheduler(),
                       runStreamClient(host, port, frame_count, payload_size));

    std::this_thread::sleep_for(std::chrono::seconds(5));
    runtime.stop();
    return 0;
}
