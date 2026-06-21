#include <galay/cpp/galay-rpc/kernel/rpc_client.h>
#include <galay/cpp/galay-rpc/kernel/rpc_server.h>
#include <galay/cpp/galay-rpc/kernel/rpc_service.h>

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
    int iterations = 1000;
    int payload_size = 128;
};

class EchoService : public RpcService {
public:
    EchoService() : RpcService("BenchLoopback") { registerMethod("echo", &EchoService::echo); }

    Task<void> echo(RpcContext& ctx)
    {
        ctx.setPayload(ctx.request().payloadView());
        co_return;
    }
};

uint16_t pickPort()
{
    const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
    return static_cast<uint16_t>(26000 + (static_cast<uint64_t>(ticks) % 20000));
}

std::string payloadString(const RpcResponse& response)
{
    return std::string(response.payload().data(), response.payload().size());
}

struct Stats {
    int attempts = 0;
    int ok = 0;
    int errors = 0;
    uint64_t bytes = 0;
    std::vector<uint64_t> latency_us;
};

Task<void> runClient(uint16_t port, const Config* config, Stats* stats)
{
    RpcClient client = RpcClientBuilder().ringBufferSize(64 * 1024).build();
    auto connected = co_await client.connect("127.0.0.1", port).timeout(500ms);
    if (!connected) {
        stats->errors = config->iterations;
        co_return;
    }

    const std::string payload(static_cast<size_t>(config->payload_size), 'u');
    stats->latency_us.reserve(static_cast<size_t>(config->iterations));

    for (int i = 0; i < config->iterations; ++i) {
        ++stats->attempts;
        const auto begin = std::chrono::steady_clock::now();
        auto call = co_await client.call("BenchLoopback", "echo", payload);
        const auto end = std::chrono::steady_clock::now();

        if (!call.has_value() ||
            !call.value().has_value() ||
            !call.value()->has_value() ||
            call.value()->value().errorCode() != RpcErrorCode::OK ||
            payloadString(call.value()->value()) != payload) {
            ++stats->errors;
            continue;
        }

        ++stats->ok;
        stats->bytes += static_cast<uint64_t>(payload.size() * 2);
        stats->latency_us.push_back(static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(end - begin).count()));
    }

    (void)co_await client.close();
}

bool runClientWithRetry(uint16_t port, const Config& config, Stats* stats)
{
    for (int attempt = 0; attempt < 50; ++attempt) {
        Stats attempt_stats;
        Runtime runtime = RuntimeBuilder().ioSchedulerCount(1).computeSchedulerCount(0).build();
        auto result = runtime.blockOn(runClient(port, &config, &attempt_stats));
        runtime.stop();
        if (!result.has_value()) {
            ++attempt_stats.errors;
        }
        if (attempt_stats.attempts > 0 && attempt_stats.ok + attempt_stats.errors > 0) {
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

Config parseArgs(int argc, char** argv)
{
    Config config;
    config.port = pickPort();
    for (int i = 1; i + 1 < argc; i += 2) {
        std::string opt = argv[i];
        std::string value = argv[i + 1];
        if (opt == "-n") config.iterations = std::stoi(value);
        else if (opt == "-s") config.payload_size = std::stoi(value);
        else if (opt == "-p") config.port = static_cast<uint16_t>(std::stoi(value));
    }
    return config;
}

} // namespace

int main(int argc, char** argv)
{
    Config config = parseArgs(argc, argv);
    auto server = RpcServerBuilder()
        .host("127.0.0.1")
        .port(config.port)
        .ioSchedulerCount(1)
        .computeSchedulerCount(0)
        .ringBufferSize(64 * 1024)
        .build();
    server.registerService(std::make_shared<EchoService>());
    server.start();

    Stats stats;
    const auto begin = std::chrono::steady_clock::now();
    const bool connected = runClientWithRetry(config.port, config, &stats);
    const auto end = std::chrono::steady_clock::now();
    server.stop();

    const double seconds = std::max(0.000001,
        std::chrono::duration<double>(end - begin).count());
    std::cout << "rpc unary loopback latency"
              << " iterations=" << config.iterations
              << " payload_bytes=" << config.payload_size
              << " connected=" << (connected ? 1 : 0)
              << " ok=" << stats.ok
              << " errors=" << stats.errors
              << " error_rate=" << std::fixed << std::setprecision(4)
              << (stats.attempts == 0 ? 1.0 : static_cast<double>(stats.errors) / stats.attempts)
              << " qps=" << std::setprecision(2) << (stats.ok / seconds)
              << " mbps=" << (stats.bytes / seconds / 1024.0 / 1024.0)
              << " p50_us=" << percentile(stats.latency_us, 0.50)
              << " p95_us=" << percentile(stats.latency_us, 0.95)
              << " p99_us=" << percentile(stats.latency_us, 0.99)
              << "\n";
    return (connected && stats.errors == 0 && stats.ok == config.iterations) ? 0 : 1;
}
