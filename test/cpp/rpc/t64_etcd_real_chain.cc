#include <galay/cpp/galay-etcd/sync/etcd_client.h>
#include <galay/cpp/galay-rpc/discovery/etcd_service_registry.h>
#include <galay/cpp/galay-rpc/kernel/rpc_managed_client.h>
#include <galay/cpp/galay-rpc/kernel/rpc_server.h>
#include <galay/cpp/galay-rpc/kernel/rpc_service.h>
#include <galay/cpp/galay-kernel/common/sleep.hpp>
#include <galay/cpp/galay-kernel/core/runtime.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace galay::kernel;
using namespace galay::rpc;

namespace {

constexpr int kSkip = 125;

class EtcdEchoService final : public RpcService {
public:
    EtcdEchoService()
        : RpcService("EtcdEcho")
    {
        registerMethod("echo", &EtcdEchoService::echo);
    }

    Task<void> echo(RpcContext& ctx)
    {
        ctx.setPayload(ctx.request().payloadView());
        co_return;
    }
};

struct TestState {
    std::atomic<bool> done{false};
    bool ok = true;
    std::string error;
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
    return static_cast<uint16_t>(43000 + (::getpid() % 8000));
}

int fail(const std::string& message)
{
    std::cerr << message << "\n";
    return 1;
}

void fail(TestState* state, std::string message)
{
    state->ok = false;
    state->error = std::move(message);
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

Task<void> runManagedCall(RpcStaticDiscovery* discovery, TestState* state)
{
    RpcManagedClientConfig config;
    config.pool.max_connections_per_endpoint = 1;
    config.pool.max_waiters_per_endpoint = 4;

    RpcManagedClient client(*discovery, config);
    bool connected = false;
    for (int attempt = 0; attempt < 100; ++attempt) {
        auto result = co_await client.call("EtcdEcho", "echo", "etcd-real");
        if (payloadEquals(result, "etcd-real")) {
            connected = true;
            break;
        }
        co_await sleep(std::chrono::milliseconds(10));
    }
    if (!connected) {
        fail(state, "managed client call through etcd-discovered endpoint failed");
    }

    auto shutdown = client.shutdown();
    if (!shutdown.has_value() && state->ok) {
        fail(state, "managed client shutdown failed");
    }
    state->done.store(true, std::memory_order_release);
    co_return;
}

} // namespace

int main()
{
    if (!integrationEnabled()) {
        std::cout << "[SKIP] set GALAY_IT_ENABLE=1 to run RPC etcd real chain integration test\n";
        return kSkip;
    }

    const std::string endpoint = etcdEndpoint();
    const std::string run_id = suffix();
    const std::string prefix = "/galay/rpc/it/" + run_id;
    const std::string instance_id = "instance-" + run_id;
    const std::string key_template = "{prefix}/custom/{service}/by/{instance}";
    const std::string expected_key = prefix + "/custom/EtcdEcho/by/" + instance_id;
    const uint16_t port = loopbackPort();

    RpcEtcdRegistryConfig registry_config;
    registry_config.endpoint = endpoint;
    registry_config.prefix = prefix;
    registry_config.key_template = key_template;
    EtcdServiceRegistry registry(registry_config);

    auto server = RpcServerBuilder()
        .host("127.0.0.1")
        .port(port)
        .ioSchedulerCount(1)
        .computeSchedulerCount(0)
        .build();
    EtcdEchoService service;
    auto registered = server.registerService(service);
    if (!registered.has_value()) {
        std::cerr << "failed to register etcd echo service: "
                  << registered.error().message() << "\n";
        return 1;
    }
    auto server_started = server.start();
    if (!server_started.has_value()) {
        std::cerr << "failed to start etcd echo server: "
                  << server_started.error().message() << "\n";
        return 1;
    }

    RpcEndpointInfo info;
    info.host = "127.0.0.1";
    info.port = port;
    info.service = "EtcdEcho";
    info.instance_id = instance_id;
    info.weight = 100;
    info.status = RpcEndpointStatus::Serving;
    info.version = "it";
    info.zone = "local";

    auto cleanup = [&] {
        (void)registry.deregisterEndpoint("EtcdEcho", instance_id);
        server.stop();
    };

    auto registered = registry.registerEndpoint(info);
    if (!registered.has_value()) {
        cleanup();
        return fail("register endpoint failed: " + registered.error().message());
    }

    galay::etcd::EtcdConfig etcd_config;
    etcd_config.endpoint = endpoint;
    etcd_config.api_prefix = "/v3";
    auto etcd = galay::etcd::EtcdClientBuilder().config(etcd_config).build();
    auto connected = etcd.connect();
    if (!connected.has_value()) {
        cleanup();
        return fail("etcd connect failed: " + connected.error().message());
    }
    auto kv = etcd.get(expected_key);
    if (!kv.has_value()) {
        cleanup();
        return fail("etcd get expected key failed: " + kv.error().message());
    }
    if (kv->empty() || kv->front().key != expected_key) {
        cleanup();
        return fail("configured etcd key template was not used");
    }

    auto discovered = registry.discover("EtcdEcho");
    if (!discovered.has_value()) {
        cleanup();
        return fail("discover failed: " + discovered.error().message());
    }
    if (discovered->size() != 1 || discovered->front().instance_id != instance_id ||
        discovered->front().port != port) {
        cleanup();
        return fail("discover did not return the registered endpoint");
    }

    RpcStaticDiscovery discovery;
    discovery.set("EtcdEcho", toManagedEndpoints(*discovered));

    Runtime runtime = RuntimeBuilder().ioSchedulerCount(1).computeSchedulerCount(0).build();
    auto runtime_started = runtime.start();
    if (!runtime_started.has_value()) {
        cleanup();
        return fail("failed to start managed client runtime: " +
                    std::string(runtime_started.error().message()));
    }

    TestState state;
    auto scheduled = runtime.spawn(runManagedCall(&discovery, &state));
    if (!scheduled.has_value()) {
        runtime.stop();
        cleanup();
        return fail("failed to schedule managed client call");
    }

    for (int i = 0; i < 500 && !state.done.load(std::memory_order_acquire); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    runtime.stop();
    if (!state.done.load(std::memory_order_acquire)) {
        cleanup();
        return fail("managed client call timed out");
    }
    if (!state.ok) {
        cleanup();
        return fail(state.error);
    }

    auto deregistered = registry.deregisterEndpoint("EtcdEcho", instance_id);
    if (!deregistered.has_value()) {
        cleanup();
        return fail("deregister endpoint failed: " + deregistered.error().message());
    }
    auto empty = registry.discover("EtcdEcho");
    if (!empty.has_value()) {
        cleanup();
        return fail("post-deregister discover failed: " + empty.error().message());
    }
    if (!empty->empty()) {
        cleanup();
        return fail("deregistered endpoint still discovered");
    }

    server.stop();
    std::cout << "RPC etcd real chain PASS\n";
    return 0;
}
