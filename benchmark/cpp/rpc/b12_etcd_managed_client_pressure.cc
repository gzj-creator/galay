#include <galay/cpp/galay-rpc/discovery/etcd_service_registry.h>
#include <galay/cpp/galay-rpc/kernel/rpc_managed_client.h>
#include <galay/cpp/galay-rpc/kernel/rpc_server.h>
#include <galay/cpp/galay-rpc/kernel/rpc_service.h>
#include <galay/cpp/galay-kernel/core/runtime.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace galay::kernel;
using namespace galay::rpc;

namespace {

constexpr int kSkip = 125;

class BenchEtcdEchoService final : public RpcService {
public:
    BenchEtcdEchoService()
        : RpcService("BenchEtcdEcho")
    {
        registerMethod("echo", &BenchEtcdEchoService::echo);
    }

    Task<void> echo(RpcContext& ctx)
    {
        ctx.setPayload(ctx.request().payloadView());
        co_return;
    }
};

struct BenchState {
    bool ok = true;
    std::string error;
    size_t completed = 0;
    size_t errors = 0;
    double total_latency_us = 0.0;
    double max_latency_us = 0.0;
};

bool integrationEnabled()
{
    const char* value = std::getenv("GALAY_IT_ENABLE");
    if (value == nullptr || value[0] == '\0') {
        return false;
    }
    const std::string enabled(value);
    return enabled == "1" || enabled == "true" || enabled == "TRUE" ||
           enabled == "yes" || enabled == "YES" || enabled == "on" ||
           enabled == "ON";
}

std::string etcdEndpoint()
{
    const char* value = std::getenv("GALAY_ETCD_ENDPOINT");
    if (value == nullptr || value[0] == '\0') {
        return "http://140.143.142.251:2379";
    }
    return value;
}

std::string suffix()
{
    const auto now = std::chrono::high_resolution_clock::now().time_since_epoch();
    return std::to_string(::getpid()) + "-" +
           std::to_string(std::chrono::duration_cast<std::chrono::microseconds>(now).count());
}

uint16_t loopbackPort()
{
    return static_cast<uint16_t>(45000 + (::getpid() % 8000));
}

int fail(const std::string& message)
{
    std::cerr << message << "\n";
    return 1;
}

bool payloadEquals(const auto& result, const std::string& expected)
{
    if (!result.has_value() || !result->has_value() || !result->value().has_value()) {
        return false;
    }
    const auto& payload = result->value()->payload();
    return result->value()->isOk() && std::string(payload.begin(), payload.end()) == expected;
}

std::vector<RpcEndpoint> toManagedEndpoints(const std::vector<RpcEndpointInfo>& infos)
{
    std::vector<RpcEndpoint> endpoints;
    for (const auto& info : infos) {
        if (info.selectable()) {
            endpoints.push_back(RpcEndpoint{info.host, info.port});
        }
    }
    return endpoints;
}

size_t parseSizeArg(int argc, char** argv, int index, size_t fallback)
{
    if (argc <= index) {
        return fallback;
    }
    return static_cast<size_t>(std::strtoull(argv[index], nullptr, 10));
}

Task<void> runPressure(RpcStaticDiscovery* discovery,
                       size_t requests,
                       BenchState* state,
                       std::atomic<size_t>* done_count)
{
    RpcManagedClientConfig config;
    config.pool.max_connections_per_endpoint = 2;
    config.pool.max_waiters_per_endpoint = 128;

    RpcManagedClient client(*discovery, config);
    for (size_t i = 0; i < requests; ++i) {
        const auto begin = std::chrono::steady_clock::now();
        auto result = co_await client.call("BenchEtcdEcho", "echo", "bench-etcd");
        const auto elapsed = std::chrono::duration<double, std::micro>(
            std::chrono::steady_clock::now() - begin).count();
        state->total_latency_us += elapsed;
        if (elapsed > state->max_latency_us) {
            state->max_latency_us = elapsed;
        }
        ++state->completed;
        if (!payloadEquals(result, "bench-etcd")) {
            ++state->errors;
        }
    }

    auto shutdown = client.shutdown();
    if (!shutdown.has_value()) {
        state->ok = false;
        state->error = "managed client shutdown failed";
    }
    done_count->fetch_add(1, std::memory_order_release);
    co_return;
}

} // namespace

int main(int argc, char** argv)
{
    if (!integrationEnabled()) {
        std::cout << "[SKIP] set GALAY_IT_ENABLE=1 to run RPC etcd managed client pressure benchmark\n";
        return kSkip;
    }

    const size_t requests = parseSizeArg(argc, argv, 1, 1000);
    const size_t client_io_schedulers = parseSizeArg(argc, argv, 2, 1);
    const size_t server_io_schedulers = parseSizeArg(argc, argv, 3, 1);
    size_t concurrency = parseSizeArg(argc, argv, 4, 1);
    if (requests == 0) {
        return fail("requests must be > 0");
    }
    if (client_io_schedulers == 0 || server_io_schedulers == 0) {
        return fail("client/server io scheduler count must be > 0");
    }
    if (concurrency == 0) {
        return fail("concurrency must be > 0");
    }
    if (concurrency > requests) {
        concurrency = requests;
    }

    const std::string endpoint = etcdEndpoint();
    const std::string run_id = suffix();
    const std::string prefix = "/galay/rpc/bench/" + run_id;
    const std::string instance_id = "instance-" + run_id;
    const uint16_t port = loopbackPort();

    RpcEtcdRegistryConfig registry_config;
    registry_config.endpoint = endpoint;
    registry_config.prefix = prefix;
    registry_config.key_template = "{prefix}/pressure/{service}/{instance}";
    EtcdServiceRegistry registry(registry_config);

    auto server = RpcServerBuilder()
        .host("127.0.0.1")
        .port(port)
        .ioSchedulerCount(server_io_schedulers)
        .computeSchedulerCount(0)
        .build();
    server.registerService(std::make_shared<BenchEtcdEchoService>());
    server.start();

    RpcEndpointInfo info;
    info.host = "127.0.0.1";
    info.port = port;
    info.service = "BenchEtcdEcho";
    info.instance_id = instance_id;
    info.weight = 100;
    info.status = RpcEndpointStatus::Serving;
    info.version = "bench";
    info.zone = "local";

    auto cleanup = [&] {
        (void)registry.deregisterEndpoint("BenchEtcdEcho", instance_id);
        server.stop();
    };

    auto registered = registry.registerEndpoint(info);
    if (!registered.has_value()) {
        cleanup();
        return fail("register endpoint failed: " + registered.error().message());
    }

    auto discovered = registry.discover("BenchEtcdEcho");
    if (!discovered.has_value()) {
        cleanup();
        return fail("discover failed: " + discovered.error().message());
    }

    RpcStaticDiscovery discovery;
    auto managed_endpoints = toManagedEndpoints(*discovered);
    if (managed_endpoints.empty()) {
        cleanup();
        return fail("discover returned no selectable endpoints");
    }
    discovery.set("BenchEtcdEcho", std::move(managed_endpoints));

    Runtime runtime = RuntimeBuilder()
        .ioSchedulerCount(client_io_schedulers)
        .computeSchedulerCount(0)
        .build();
    runtime.start();

    std::vector<BenchState> states(concurrency);
    std::vector<JoinHandle<void>> handles;
    handles.reserve(concurrency);
    std::atomic<size_t> done_count{0};
    const auto begin = std::chrono::steady_clock::now();
    const size_t base_requests = requests / concurrency;
    const size_t extra_requests = requests % concurrency;
    for (size_t i = 0; i < concurrency; ++i) {
        const size_t task_requests = base_requests + (i < extra_requests ? 1 : 0);
        auto scheduled = runtime.spawn(runPressure(&discovery, task_requests, &states[i], &done_count));
        if (!scheduled.has_value()) {
            runtime.stop();
            cleanup();
            return fail("failed to schedule pressure task");
        }
        handles.push_back(std::move(*scheduled));
    }

    for (int i = 0; i < 12000 && done_count.load(std::memory_order_acquire) < concurrency; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    if (done_count.load(std::memory_order_acquire) < concurrency) {
        runtime.stop();
        cleanup();
        return fail("pressure benchmark timed out");
    }

    for (auto& handle : handles) {
        auto joined = handle.join();
        if (!joined.has_value()) {
            runtime.stop();
            cleanup();
            return fail("pressure task join failed");
        }
    }

    const double seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - begin).count();
    runtime.stop();

    BenchState total;
    for (const auto& state : states) {
        if (!state.ok) {
            cleanup();
            return fail(state.error);
        }
        total.completed += state.completed;
        total.errors += state.errors;
        total.total_latency_us += state.total_latency_us;
        if (state.max_latency_us > total.max_latency_us) {
            total.max_latency_us = state.max_latency_us;
        }
    }

    auto deregistered = registry.deregisterEndpoint("BenchEtcdEcho", instance_id);
    if (!deregistered.has_value()) {
        cleanup();
        return fail("deregister endpoint failed: " + deregistered.error().message());
    }
    server.stop();

    const double qps = seconds > 0.0 ? static_cast<double>(total.completed) / seconds : 0.0;
    const double avg_latency = total.completed > 0
        ? total.total_latency_us / static_cast<double>(total.completed)
        : 0.0;

    std::cout << "RPC etcd managed client pressure\n"
              << "requests=" << requests << "\n"
              << "client_io_schedulers=" << client_io_schedulers << "\n"
              << "server_io_schedulers=" << server_io_schedulers << "\n"
              << "concurrency=" << concurrency << "\n"
              << "completed=" << total.completed << "\n"
              << "qps=" << qps << "\n"
              << "errors=" << total.errors << "\n"
              << "avg_latency_us=" << avg_latency << "\n"
              << "max_latency_us=" << total.max_latency_us << "\n";

    return total.errors == 0 ? 0 : 1;
}
