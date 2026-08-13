/**
 * @file b8_v2_subscription_broadcast_pressure.cc
 * @brief Real SSE subscription broadcast pressure benchmark.
 */

#include <galay/cpp/galay-kernel/core/runtime.h>
#include <galay/cpp/galay-mcp/v2/client/client.h>
#include <galay/cpp/galay-mcp/v2/server/http_server.h>

#include <arpa/inet.h>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <netinet/in.h>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>

namespace {

using namespace std::chrono_literals;

uint16_t pickPort()
{
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return 0;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    if (::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) != 1) {
        ::close(fd);
        return 0;
    }
    address.sin_port = 0;
    if (::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        ::close(fd);
        return 0;
    }
    socklen_t length = sizeof(address);
    const bool ok = ::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length) == 0;
    const auto value = static_cast<uint16_t>(ntohs(address.sin_port));
    ::close(fd);
    return ok ? value : 0;
}

} // namespace

int main(int argc, char** argv)
{
    std::size_t iterations = 200'000;
    if (argc > 1) {
        const std::string_view text(argv[1]);
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), iterations);
        if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || iterations == 0) {
            std::cerr << "usage: benchmark_mcp_v2_subscription_broadcast_pressure [positive-iterations]\n";
            return 2;
        }
    }

    const auto port = pickPort();
    if (port == 0) {
        std::cerr << "failed to pick benchmark port\n";
        return 1;
    }

    galay::mcp::v2::McpHttpServer server("127.0.0.1", port, 1, 0);
    server.addTool("pressure", "pressure", "{}",
                   [](const galay::mcp::JsonElement&,
                      std::expected<galay::mcp::JsonString, galay::mcp::McpError>& result)
                       -> galay::kernel::Task<void> {
                       result = "ok";
                       co_return;
                   });
    server.addResource("mem://pressure", "pressure", "pressure", "text/plain",
                       [](const std::string&,
                          std::expected<std::string, galay::mcp::McpError>& result)
                           -> galay::kernel::Task<void> {
                           result = "ok";
                           co_return;
                       });
    server.addPrompt("pressure", "pressure", {},
                     [](const std::string&, const galay::mcp::JsonElement&,
                        std::expected<galay::mcp::JsonString, galay::mcp::McpError>& result)
                         -> galay::kernel::Task<void> {
                         result = R"({"messages":[]})";
                         co_return;
                     });

    std::thread serverThread([&server] { server.start(); });
    const auto serverDeadline = std::chrono::steady_clock::now() + 3s;
    while (!server.isRunning() && std::chrono::steady_clock::now() < serverDeadline) {
        std::this_thread::sleep_for(1ms);
    }
    if (!server.isRunning()) {
        server.stop();
        serverThread.join();
        std::cerr << "benchmark server did not start\n";
        return 1;
    }

    galay::kernel::Runtime runtime =
        galay::kernel::RuntimeBuilder().ioSchedulerCount(1).computeSchedulerCount(0).build();
    const auto runtimeStarted = runtime.start();
    if (!runtimeStarted) {
        server.stop();
        serverThread.join();
        std::cerr << "benchmark runtime did not start\n";
        return 1;
    }

    galay::mcp::v2::McpHttpClient client(
        runtime, "http://127.0.0.1:" + std::to_string(port) + "/mcp");
    galay::mcp::v2::SubscriptionFilter filter;
    filter.toolsListChanged = true;
    filter.resourcesListChanged = true;
    filter.promptsListChanged = true;
    filter.resourceSubscriptions.push_back("mem://pressure");

    std::expected<galay::mcp::v2::SubscriptionFilter, galay::mcp::McpError> listenResult =
        std::unexpected(galay::mcp::McpError::invalidResponse("pending acknowledgement"));
    std::atomic<std::size_t> received{0};
    auto listener = client.listen(
        filter,
        [&received](std::string) {
            received.fetch_add(1, std::memory_order_acq_rel);
            return true;
        },
        listenResult);
    auto listenerHandle = runtime.spawn(std::move(listener));
    if (!listenerHandle) {
        server.stop();
        serverThread.join();
        runtime.stop();
        std::cerr << "failed to schedule benchmark subscription\n";
        return 1;
    }

    const auto acknowledgementDeadline = std::chrono::steady_clock::now() + 3s;
    bool warmupEnqueued = false;
    while (!warmupEnqueued && std::chrono::steady_clock::now() < acknowledgementDeadline) {
        warmupEnqueued = server.notifyToolsListChanged() == 1;
        if (warmupEnqueued) break;
        std::this_thread::sleep_for(1ms);
    }
    while (received.load(std::memory_order_acquire) == 0 &&
           std::chrono::steady_clock::now() < acknowledgementDeadline) {
        std::this_thread::sleep_for(1ms);
    }
    if (!warmupEnqueued || received.load(std::memory_order_acquire) == 0) {
        server.stop();
        (void)listenerHandle->join();
        serverThread.join();
        runtime.stop();
        std::cerr << "subscription acknowledgement timed out\n";
        return 1;
    }

    const std::size_t publishCalls = iterations * 4;
    const auto start = std::chrono::steady_clock::now();
    std::size_t enqueued = 0;
    for (std::size_t i = 0; i < iterations; ++i) {
        enqueued += server.notifyToolsListChanged();
        enqueued += server.notifyResourcesListChanged();
        enqueued += server.notifyPromptsListChanged();
        enqueued += server.notifyResourceUpdated("mem://pressure");
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - start).count();

    const auto listenerDeadline = std::chrono::steady_clock::now() + 5s;
    while (received.load(std::memory_order_acquire) < enqueued + 1 &&
           std::chrono::steady_clock::now() < listenerDeadline) {
        std::this_thread::sleep_for(1ms);
    }
    const auto callbackEvents = received.load(std::memory_order_acquire) - 1;
    server.stop();
    (void)listenerHandle->join();
    serverThread.join();
    runtime.stop();

    if (elapsed <= 0 || callbackEvents != enqueued || !listenResult.has_value()) return 1;
    std::cout << "MCP v2 subscription iterations: " << iterations << '\n'
              << "Broadcast calls: " << publishCalls << '\n'
              << "Enqueued events: " << enqueued << '\n'
              << "Callback events: " << callbackEvents << '\n'
              << "Publish throughput: "
              << (static_cast<double>(publishCalls) * 1'000'000'000.0 / elapsed)
              << " events/s\n"
              << "Average publish: "
              << (static_cast<double>(elapsed) / publishCalls)
              << " ns/event\n";
    return 0;
}
