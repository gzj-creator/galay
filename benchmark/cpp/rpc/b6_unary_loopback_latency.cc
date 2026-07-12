/**
 * @file b6_unary_loopback_latency.cc
 * @brief RPC unary loopback 延迟 smoke benchmark
 */

#include <galay/cpp/galay-rpc/kernel/rpc_client.h>
#include <galay/cpp/galay-rpc/kernel/rpc_server.h>
#include <galay/cpp/galay-rpc/kernel/rpc_service.h>
#include <galay/cpp/galay-kernel/common/sleep.hpp>
#include <galay/cpp/galay-kernel/core/runtime.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>

using namespace galay::kernel;
using namespace galay::rpc;

namespace {

class BenchLoopbackService final : public RpcService {
public:
    BenchLoopbackService()
        : RpcService("BenchLoopbackService")
    {
        registerMethod("echo", &BenchLoopbackService::echo);
    }

    Task<void> echo(RpcContext& ctx)
    {
        ctx.setPayload(ctx.request().payloadView());
        co_return;
    }
};

struct BenchResult {
    bool done = false;
    size_t operations = 0;
    size_t errors = 0;
    double elapsed_sec = 0.0;
    std::vector<uint64_t> latencies_us;
};

uint16_t loopbackPort()
{
    return static_cast<uint16_t>(24000 + (::getpid() % 20000));
}

Task<void> runBenchmarkClient(uint16_t port, size_t iterations, BenchResult* result)
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
        result->errors = iterations;
        result->done = true;
        co_return;
    }

    result->latencies_us.reserve(iterations);
    const std::string payload = "benchmark-payload";
    const auto bench_start = std::chrono::steady_clock::now();
    for (size_t i = 0; i < iterations; ++i) {
        const auto start = std::chrono::steady_clock::now();
        auto call_result = co_await client.call("BenchLoopbackService", "echo", payload);
        const auto end = std::chrono::steady_clock::now();
        result->latencies_us.push_back(
            static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(end - start).count()));

        const RpcResponse* response = nullptr;
        if (call_result.has_value() &&
            call_result.value().has_value() &&
            call_result.value()->has_value()) {
            response = &call_result.value()->value();
        }
        if (response == nullptr || !response->isOk() ||
            std::string(response->payload().begin(), response->payload().end()) != payload) {
            ++result->errors;
            continue;
        }
        ++result->operations;
    }
    const auto bench_end = std::chrono::steady_clock::now();
    result->elapsed_sec =
        std::chrono::duration_cast<std::chrono::duration<double>>(bench_end - bench_start).count();

    co_await client.close();
    result->done = true;
    co_return;
}

uint64_t percentile(const std::vector<uint64_t>& values, double p)
{
    if (values.empty()) {
        return 0;
    }
    const size_t idx = std::min(values.size() - 1,
                                static_cast<size_t>((values.size() - 1) * p));
    return values[idx];
}

} // namespace

int main(int argc, char* argv[])
{
    size_t iterations = 1000;
    if (argc > 1) {
        iterations = std::max<size_t>(1, std::strtoull(argv[1], nullptr, 10));
    }

    const uint16_t port = loopbackPort();
    auto server = RpcServerBuilder()
        .host("127.0.0.1")
        .port(port)
        .ioSchedulerCount(1)
        .computeSchedulerCount(0)
        .build();
    BenchLoopbackService service;
    auto registered = server.registerService(service);
    if (!registered.has_value()) {
        std::cerr << "failed to register loopback benchmark service: "
                  << registered.error().message() << "\n";
        return 1;
    }
    auto server_started = server.start();
    if (!server_started.has_value()) {
        std::cerr << "failed to start loopback benchmark server: "
                  << server_started.error().message() << "\n";
        return 1;
    }

    Runtime runtime = RuntimeBuilder().ioSchedulerCount(1).computeSchedulerCount(0).build();
    auto runtime_started = runtime.start();
    if (!runtime_started.has_value()) {
        server.stop();
        std::cerr << "failed to start loopback benchmark runtime: "
                  << runtime_started.error().message() << "\n";
        return 1;
    }

    BenchResult result;
    if (!scheduleTask(runtime.getNextIOScheduler(), runBenchmarkClient(port, iterations, &result))) {
        runtime.stop();
        server.stop();
        std::cerr << "failed to schedule benchmark client\n";
        return 1;
    }

    for (int i = 0; i < 600 && !result.done; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    runtime.stop();
    server.stop();

    if (!result.done) {
        std::cerr << "benchmark timed out\n";
        return 1;
    }

    std::sort(result.latencies_us.begin(), result.latencies_us.end());
    const double ops_per_sec = result.elapsed_sec > 0.0
        ? static_cast<double>(result.operations) / result.elapsed_sec
        : 0.0;

    std::cout << "iterations=" << iterations << "\n"
              << "operations=" << result.operations << "\n"
              << "errors=" << result.errors << "\n"
              << "elapsed_sec=" << result.elapsed_sec << "\n"
              << "ops_per_sec=" << ops_per_sec << "\n"
              << "latency_us_p50=" << percentile(result.latencies_us, 0.50) << "\n"
              << "latency_us_p90=" << percentile(result.latencies_us, 0.90) << "\n"
              << "latency_us_p99=" << percentile(result.latencies_us, 0.99) << "\n"
              << "latency_us_max=" << percentile(result.latencies_us, 1.00) << "\n";

    return result.errors == 0 && result.operations == iterations ? 0 : 1;
}
