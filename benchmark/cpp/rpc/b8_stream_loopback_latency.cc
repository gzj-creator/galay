#include "../common/benchmark_environment.h"

#include <galay/cpp/galay-rpc/kernel/rpc_client.h>
#include <galay/cpp/galay-rpc/kernel/rpc_service.h>
#include <galay/cpp/galay-rpc/kernel/streamsvc.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

using namespace galay::kernel;
using namespace galay::rpc;
using namespace std::chrono_literals;

namespace {

struct Config {
    uint16_t port = 0;
    int frames = 1000;
    int payload_size = 128;
};

struct Stats {
    int sent = 0;
    int echoed = 0;
    int errors = 0;
    uint64_t bytes = 0;
    std::vector<uint64_t> latency_us;
};

uint16_t pick_port()
{
    const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
    return static_cast<uint16_t>(30000 + (static_cast<uint64_t>(ticks) % 12000));
}

class StreamBenchService : public RpcService {
public:
    StreamBenchService() : RpcService("StreamBenchLoopback")
    {
        register_stream_method("echo", &StreamBenchService::echo);
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
            (void)co_await stream.send_cancel();
            co_return;
        }
    }
};

Task<void> run_client(uint16_t port, const Config* config, Stats* stats)
{
    RpcClient client = RpcClientBuilder().ring_buffer_size(256 * 1024).build();
    auto connected = co_await client.connect("127.0.0.1", port);
    if (!connected.has_value()) {
        stats->errors = config->frames;
        co_return;
    }

    auto stream_result = client.create_stream(1, "StreamBenchLoopback", "echo");
    if (!stream_result.has_value()) {
        stats->errors = config->frames;
        (void)co_await client.close();
        co_return;
    }
    auto stream = std::move(stream_result.value());

    auto send_result = co_await stream.send_init().timeout(500ms);
    if (!send_result.has_value()) {
        stats->errors = config->frames;
        (void)co_await client.close();
        co_return;
    }

    StreamMessage ack;
    auto recv_result = co_await stream.read(ack).timeout(500ms);
    if (!recv_result.has_value() || ack.message_type() != RpcMessageType::STREAM_INIT_ACK) {
        stats->errors = config->frames;
        (void)co_await client.close();
        co_return;
    }

    std::string payload(static_cast<size_t>(config->payload_size), 's');
    stats->latency_us.reserve(static_cast<size_t>(config->frames));

    for (int i = 0; i < config->frames; ++i) {
        ++stats->sent;
        const auto begin = std::chrono::steady_clock::now();
        auto frame_send = co_await stream.send_data(payload).timeout(1s);
        if (!frame_send.has_value()) {
            ++stats->errors;
            continue;
        }

        StreamMessage echo;
        recv_result = co_await stream.read(echo).timeout(1s);
        const auto end = std::chrono::steady_clock::now();
        if (!recv_result.has_value() ||
            echo.message_type() != RpcMessageType::STREAM_DATA ||
            !echo.payload_equals(payload)) {
            ++stats->errors;
            continue;
        }

        ++stats->echoed;
        stats->bytes += static_cast<uint64_t>(payload.size() * 2);
        stats->latency_us.push_back(static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(end - begin).count()));
    }

    (void)co_await stream.send_end().timeout(1s);
    StreamMessage end_frame;
    (void)co_await stream.read(end_frame).timeout(1s);
    (void)co_await client.close();
}

bool run_client_with_retry(uint16_t port, const Config& config, Stats* stats)
{
    for (int attempt = 0; attempt < 50; ++attempt) {
        Stats attempt_stats;
        Runtime runtime = RuntimeBuilder().io_scheduler_count(1).parallel_scheduler_count(0).build();
        auto result = runtime.block_on_io(run_client(port, &config, &attempt_stats));
        runtime.stop();
        if (!result.has_value()) {
            ++attempt_stats.errors;
        }
        if (attempt_stats.sent > 0) {
            *stats = std::move(attempt_stats);
            return true;
        }
        std::this_thread::sleep_for(10ms);
    }
    return false;
}

uint64_t percentile(std::vector<uint64_t> values, double p)
{
    if (values.empty()) {
        return 0;
    }
    std::sort(values.begin(), values.end());
    const size_t index = std::min(values.size() - 1,
                                  static_cast<size_t>((values.size() - 1) * p));
    return values[index];
}

Config parse_args(int argc, char** argv)
{
    Config config;
    config.port = pick_port();
    for (int i = 1; i + 1 < argc; i += 2) {
        std::string opt = argv[i];
        std::string value = argv[i + 1];
        if (opt == "-n") config.frames = std::stoi(value);
        else if (opt == "-s") config.payload_size = std::stoi(value);
        else if (opt == "-p") config.port = static_cast<uint16_t>(std::stoi(value));
    }
    return config;
}

} // namespace

int main(int argc, char** argv)
{
    if (!galay::benchmark::initialize_benchmark_environment()) {
        return 1;
    }

    Config config = parse_args(argc, argv);
    auto server = RpcStreamServerBuilder()
        .host("127.0.0.1")
        .port(config.port)
        .io_scheduler_count(1)
        .parallel_scheduler_count(0)
        .ring_buffer_size(256 * 1024)
        .build();
    StreamBenchService service;
    auto registered = server.register_service(service);
    if (!registered.has_value()) {
        std::cerr << "failed to register stream loopback benchmark service: "
                  << registered.error().message() << "\n";
        return 1;
    }
    auto started = server.start();
    if (!started.has_value()) {
        std::cerr << "failed to start stream loopback benchmark server: "
                  << started.error().message() << "\n";
        return 1;
    }

    Stats stats;
    const auto begin = std::chrono::steady_clock::now();
    const bool connected = run_client_with_retry(config.port, config, &stats);
    const auto end = std::chrono::steady_clock::now();
    server.stop();

    const double seconds = std::max(0.000001,
        std::chrono::duration<double>(end - begin).count());
    const int total = stats.echoed + stats.errors;
    std::cout << "rpc stream loopback latency"
              << " frames=" << config.frames
              << " payload_bytes=" << config.payload_size
              << " connected=" << (connected ? 1 : 0)
              << " echoed=" << stats.echoed
              << " errors=" << stats.errors
              << " error_rate=" << std::fixed << std::setprecision(4)
              << (total == 0 ? 1.0 : static_cast<double>(stats.errors) / total)
              << " frames_per_s=" << std::setprecision(2) << (stats.echoed / seconds)
              << " mbps=" << (stats.bytes / seconds / 1024.0 / 1024.0)
              << " p50_us=" << percentile(stats.latency_us, 0.50)
              << " p95_us=" << percentile(stats.latency_us, 0.95)
              << " p99_us=" << percentile(stats.latency_us, 0.99)
              << "\n";
    return (connected && stats.errors == 0 && stats.echoed == config.frames) ? 0 : 1;
}
