#include <galay/cpp/galay-rpc/kernel/rpc_managed_client.h>
#include <galay/cpp/galay-rpc/kernel/rpc_server.h>
#include <galay/cpp/galay-rpc/kernel/rpc_service.h>
#include <galay/cpp/galay-kernel/common/sleep.hpp>
#include <galay/cpp/galay-kernel/core/runtime.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <expected>
#include <iostream>
#include <string>
#include <thread>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

using namespace galay::kernel;
using namespace galay::rpc;

namespace {

class ManagedService final : public RpcService {
public:
    ManagedService()
        : RpcService("ManagedService")
    {
        register_method("echo", &ManagedService::echo);
    }

    Task<void> echo(RpcContext& ctx)
    {
        ctx.set_payload(ctx.request().payload_view());
        co_return;
    }
};

struct TestState {
    std::atomic<bool> done{false};
    bool ok = true;
    std::string error;
};

std::expected<uint16_t, std::string> loopback_port()
{
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return std::unexpected(std::strerror(errno));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t length = sizeof(address);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&address), length) != 0 ||
        ::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
        std::string error = std::strerror(errno);
        if (::close(fd) != 0) error += std::string("; close: ") + std::strerror(errno);
        return std::unexpected(std::move(error));
    }
    if (::close(fd) != 0) return std::unexpected(std::strerror(errno));
    return ntohs(address.sin_port);
}

void fail(TestState* state, std::string message)
{
    state->ok = false;
    state->error = std::move(message);
}

template<typename AwaitResult>
bool payload_equals(const AwaitResult& result, const std::string& expected)
{
    if (!result.has_value() || !result->has_value() || !result->value().has_value()) {
        return false;
    }
    const auto& payload = result->value()->payload();
    return result->value()->is_ok() && std::string(payload.begin(), payload.end()) == expected;
}

Task<void> run_managed_checks(uint16_t port, TestState* state)
{
    RpcStaticDiscovery discovery;
    discovery.set("ManagedService", {
        RpcEndpoint{"127.0.0.1", static_cast<uint16_t>(port + 1)},
        RpcEndpoint{"127.0.0.1", port},
    });

    RpcManagedClientConfig config;
    config.pool.max_connections_per_endpoint = 1;
    config.pool.max_waiters_per_endpoint = 2;

    RpcManagedClient client(discovery, config);
    auto endpoints = client.refresh("ManagedService");
    if (!endpoints.has_value() || endpoints->size() != 2) {
        fail(state, "fake discovery did not return two endpoints");
        state->done.store(true, std::memory_order_release);
        co_return;
    }

    auto selected_a = client.select_endpoint("ManagedService");
    auto selected_b = client.select_endpoint("ManagedService");
    if (!selected_a.has_value() || !selected_b.has_value() ||
        selected_a->port != static_cast<uint16_t>(port + 1) || selected_b->port != port) {
        fail(state, "round-robin endpoint selection failed");
        state->done.store(true, std::memory_order_release);
        co_return;
    }

    client.mark_endpoint_unavailable(*selected_a);
    auto selected_after_failure = client.select_endpoint("ManagedService");
    if (!selected_after_failure.has_value() || selected_after_failure->port != port) {
        fail(state, "endpoint failure did not select next allowed endpoint");
        state->done.store(true, std::memory_order_release);
        co_return;
    }

    client.mark_endpoint_unavailable(*selected_after_failure);
    auto recovered_after_all_unavailable = client.select_endpoint("ManagedService");
    if (!recovered_after_all_unavailable.has_value()) {
        fail(state, "all-unavailable endpoints were not reopened for transient recovery");
        state->done.store(true, std::memory_order_release);
        co_return;
    }

    bool connected = false;
    for (int attempt = 0; attempt < 100; ++attempt) {
        auto result = co_await client.call("ManagedService", "echo", "managed");
        if (payload_equals(result, "managed")) {
            connected = true;
            break;
        }
        co_await sleep(std::chrono::milliseconds(10));
    }
    if (!connected) {
        fail(state, "real loopback managed call failed");
        state->done.store(true, std::memory_order_release);
        co_return;
    }

    RpcClient direct;
    auto connect_result = co_await direct.connect("127.0.0.1", port);
    if (!connect_result.has_value()) {
        fail(state, "existing RpcClient direct connect failed");
        state->done.store(true, std::memory_order_release);
        co_return;
    }
    if (!connect_result->has_value()) {
        fail(state, "existing RpcClient direct connect returned IO error");
        state->done.store(true, std::memory_order_release);
        co_return;
    }
    auto direct_result = co_await direct.call("ManagedService", "echo", "direct");
    co_await direct.close();
    if (!payload_equals(direct_result, "direct")) {
        fail(state, "existing RpcClient direct call failed");
        state->done.store(true, std::memory_order_release);
        co_return;
    }

    auto shutdown = client.shutdown();
    if (!shutdown.has_value()) {
        fail(state, "managed client shutdown failed");
        state->done.store(true, std::memory_order_release);
        co_return;
    }

    state->done.store(true, std::memory_order_release);
    co_return;
}

} // namespace

int main()
{
    const auto selected_port = loopback_port();
    if (!selected_port) {
        std::cerr << "failed to select managed server port: " << selected_port.error() << '\n';
        return 1;
    }
    const uint16_t port = *selected_port;

    auto server = RpcServerBuilder()
        .host("127.0.0.1")
        .port(port)
        .io_scheduler_count(1)
        .parallel_scheduler_count(0)
        .build();
    ManagedService service;
    auto registered = server.register_service(service);
    if (!registered.has_value()) {
        std::cerr << "failed to register managed service: "
                  << registered.error().message() << "\n";
        return 1;
    }
    auto server_started = server.start();
    if (!server_started.has_value()) {
        std::cerr << "failed to start managed server: "
                  << server_started.error().message() << "\n";
        return 1;
    }

    Runtime runtime = RuntimeBuilder().io_scheduler_count(1).parallel_scheduler_count(0).build();
    auto runtime_started = runtime.start();
    if (!runtime_started.has_value()) {
        server.stop();
        std::cerr << "failed to start managed runtime: "
                  << runtime_started.error().message() << "\n";
        return 1;
    }

    TestState state;
    auto scheduled = runtime.spawn_io(run_managed_checks(port, &state));
    if (!scheduled.has_value()) {
        runtime.stop();
        server.stop();
        std::cerr << "failed to schedule managed client checks\n";
        return 1;
    }

    for (int i = 0; i < 500 && !state.done.load(std::memory_order_acquire); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    runtime.stop();
    server.stop();

    if (!state.done.load(std::memory_order_acquire)) {
        std::cerr << "managed client test timed out\n";
        return 1;
    }
    if (!state.ok) {
        std::cerr << state.error << "\n";
        return 1;
    }

    std::cout << "RPC managed client PASS\n";
    return 0;
}
