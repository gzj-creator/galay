#include <galay/cpp/galay-rpc/kernel/rpc_channel.h>
#include <galay/cpp/galay-rpc/kernel/rpc_server.h>
#include <galay/cpp/galay-rpc/kernel/rpc_service.h>
#include <galay/cpp/galay-kernel/common/sleep.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

using namespace galay::kernel;
using namespace galay::rpc;
using namespace std::chrono_literals;

namespace {

struct Config {
    uint16_t port = 0;
    int iterations = 1000;
    int concurrency = 32;
    int payload_size = 128;
};

struct Stats {
    std::atomic<int> attempts{0};
    std::atomic<int> ok{0};
    std::atomic<int> errors{0};
    std::atomic<uint64_t> bytes{0};
    std::vector<uint64_t> latency_us;
};

class ChannelBenchService : public RpcService {
public:
    ChannelBenchService() : RpcService("ChannelBench") { registerMethod("echo", &ChannelBenchService::echo); }

    Task<void> echo(RpcContext& ctx)
    {
        ctx.setPayload(ctx.request().payloadView());
        co_return;
    }
};

uint16_t pickPort()
{
    const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
    return static_cast<uint16_t>(30000 + (static_cast<uint64_t>(ticks) % 12000));
}

std::string payloadString(const RpcResponse& response)
{
    return std::string(response.payload().data(), response.payload().size());
}

Task<void> callWorker(RpcChannel* channel,
                      const std::string* payload,
                      std::atomic<int>* next_index,
                      const Config* config,
                      Stats* stats,
                      std::vector<uint64_t>* worker_latencies)
{
    for (;;) {
        const int index = next_index->fetch_add(1, std::memory_order_relaxed);
        if (index >= config->iterations) {
            co_return;
        }

        stats->attempts.fetch_add(1, std::memory_order_relaxed);
        const auto begin = std::chrono::steady_clock::now();
        auto task_result = co_await channel->call("ChannelBench", "echo", *payload);
        const auto end = std::chrono::steady_clock::now();

        if (!task_result.has_value() || !task_result.value().has_value() ||
            !task_result.value().value().has_value() ||
            task_result.value().value()->errorCode() != RpcErrorCode::OK ||
            payloadString(task_result.value().value().value()) != *payload) {
            stats->errors.fetch_add(1, std::memory_order_relaxed);
            continue;
        }

        stats->ok.fetch_add(1, std::memory_order_relaxed);
        stats->bytes.fetch_add(static_cast<uint64_t>(payload->size() * 2), std::memory_order_relaxed);
        worker_latencies->push_back(static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(end - begin).count()));
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

Task<void> runChannelBench(uint16_t port, const Config* config, Stats* stats, bool* connected)
{
    RpcChannelConfig channel_config;
    channel_config.host = "127.0.0.1";
    channel_config.port = port;
    channel_config.ring_buffer_size = 128 * 1024;
    channel_config.max_outstanding_requests = static_cast<size_t>(std::max(1, config->concurrency));
    channel_config.connect_timeout = 500ms;

    RpcChannel channel(channel_config);
    auto connect_task = co_await connectWithRetry(&channel);
    if (!connect_task.has_value()) {
        *connected = false;
        co_return;
    }
    *connected = true;

    auto runtime = RuntimeHandle::current();
    if (!runtime.has_value()) {
        stats->errors.store(config->iterations, std::memory_order_relaxed);
        (void)co_await channel.shutdown();
        co_return;
    }

    const std::string payload(static_cast<size_t>(config->payload_size), 'c');
    std::atomic<int> next_index{0};
    std::vector<std::vector<uint64_t>> worker_latencies(static_cast<size_t>(config->concurrency));
    std::vector<JoinHandle<void>> joins;
    joins.reserve(static_cast<size_t>(config->concurrency));

    for (int i = 0; i < config->concurrency; ++i) {
        auto scheduled = runtime->spawn(callWorker(&channel,
                                                   &payload,
                                                   &next_index,
                                                   config,
                                                   stats,
                                                   &worker_latencies[static_cast<size_t>(i)]));
        if (scheduled.has_value()) {
            joins.push_back(std::move(scheduled.value()));
        } else {
            stats->errors.fetch_add(1, std::memory_order_relaxed);
        }
    }

    while (stats->ok.load(std::memory_order_relaxed) +
               stats->errors.load(std::memory_order_relaxed) <
           config->iterations) {
        co_await galay::kernel::sleep(1ms);
    }

    stats->latency_us.clear();
    for (auto& local : worker_latencies) {
        stats->latency_us.insert(stats->latency_us.end(), local.begin(), local.end());
    }
    const auto status = channel.status();
    if (status.connections_established != 1) {
        stats->errors.fetch_add(config->iterations, std::memory_order_relaxed);
    }
    (void)co_await channel.shutdown();
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
        const std::string opt = argv[i];
        const std::string value = argv[i + 1];
        if (opt == "-n") config.iterations = std::stoi(value);
        else if (opt == "-c") config.concurrency = std::stoi(value);
        else if (opt == "-s") config.payload_size = std::stoi(value);
        else if (opt == "-p") config.port = static_cast<uint16_t>(std::stoi(value));
    }
    config.iterations = std::max(1, config.iterations);
    config.concurrency = std::max(1, config.concurrency);
    config.payload_size = std::max(0, config.payload_size);
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
        .ringBufferSize(128 * 1024)
        .build();
    server.registerService(std::make_shared<ChannelBenchService>());
    server.start();

    Stats stats;
    const auto begin = std::chrono::steady_clock::now();
    bool connected = false;
    Runtime runtime = RuntimeBuilder().ioSchedulerCount(1).computeSchedulerCount(0).build();
    auto bench_result = runtime.blockOn(runChannelBench(config.port, &config, &stats, &connected));
    runtime.stop();
    if (!bench_result.has_value()) {
        stats.errors.fetch_add(1, std::memory_order_relaxed);
    }
    const auto end = std::chrono::steady_clock::now();
    server.stop();

    const int attempts = stats.attempts.load(std::memory_order_relaxed);
    const int ok = stats.ok.load(std::memory_order_relaxed);
    const int errors = stats.errors.load(std::memory_order_relaxed);
    const uint64_t bytes = stats.bytes.load(std::memory_order_relaxed);
    const double seconds = std::max(0.000001,
        std::chrono::duration<double>(end - begin).count());

    std::cout << "rpc channel unary concurrency"
              << " iterations=" << config.iterations
              << " concurrency=" << config.concurrency
              << " payload_bytes=" << config.payload_size
              << " connected=" << (connected ? 1 : 0)
              << " ok=" << ok
              << " errors=" << errors
              << " error_rate=" << std::fixed << std::setprecision(4)
              << (attempts == 0 ? 1.0 : static_cast<double>(errors) / attempts)
              << " qps=" << std::setprecision(2) << (ok / seconds)
              << " mbps=" << (bytes / seconds / 1024.0 / 1024.0)
              << " p50_us=" << percentile(stats.latency_us, 0.50)
              << " p95_us=" << percentile(stats.latency_us, 0.95)
              << " p99_us=" << percentile(stats.latency_us, 0.99)
              << "\n";
    return (connected && errors == 0 && ok == config.iterations) ? 0 : 1;
}
