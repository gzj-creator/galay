import galay.rpc;

#include <atomic>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>

using namespace galay::kernel;
using namespace galay::rpc;

namespace {

class ManagedExampleService final : public RpcService {
public:
    ManagedExampleService()
        : RpcService("ManagedExampleService")
    {
        registerMethod("echo", &ManagedExampleService::echo);
    }

    Task<void> echo(RpcContext& ctx)
    {
        ctx.setPayload(ctx.request().payloadView());
        co_return;
    }
};

std::atomic<bool> g_done{false};
std::atomic<bool> g_ok{false};

Task<void> runClient(uint16_t port)
{
    RpcStaticDiscovery discovery;
    discovery.set("ManagedExampleService", {RpcEndpoint{"127.0.0.1", port}});

    RpcManagedClient client(discovery);
    auto result = co_await client.call("ManagedExampleService", "echo", "hello managed client");
    if (result.has_value() && result->has_value() && result->value().has_value()) {
        const auto& payload = result->value()->payload();
        g_ok.store(std::string(payload.begin(), payload.end()) == "hello managed client",
                   std::memory_order_release);
    }
    client.shutdown();
    g_done.store(true, std::memory_order_release);
    co_return;
}

} // namespace

int main()
{
    constexpr uint16_t port = 39052;

    auto server = RpcServerBuilder()
        .host("127.0.0.1")
        .port(port)
        .ioSchedulerCount(1)
        .computeSchedulerCount(0)
        .build();
    ManagedExampleService service;
    auto registered = server.registerService(service);
    if (!registered.has_value()) {
        std::cerr << "failed to register managed example service: "
                  << registered.error().message() << "\n";
        return 1;
    }
    auto server_started = server.start();
    if (!server_started.has_value()) {
        std::cerr << "failed to start managed example server: "
                  << server_started.error().message() << "\n";
        return 1;
    }

    Runtime runtime = RuntimeBuilder().ioSchedulerCount(1).computeSchedulerCount(0).build();
    auto runtime_started = runtime.start();
    if (!runtime_started.has_value()) {
        server.stop();
        std::cerr << "failed to start managed example runtime: "
                  << runtime_started.error().message() << "\n";
        return 1;
    }
    auto scheduled = runtime.spawn(runClient(port));
    if (!scheduled.has_value()) {
        runtime.stop();
        server.stop();
        std::cerr << "failed to schedule managed client import example\n";
        return 1;
    }

    for (int i = 0; i < 200 && !g_done.load(std::memory_order_acquire); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    runtime.stop();
    server.stop();
    if (!g_done.load(std::memory_order_acquire) || !g_ok.load(std::memory_order_acquire)) {
        std::cerr << "managed client import example failed\n";
        return 1;
    }

    std::cout << "managed client import example PASS\n";
    return 0;
}
