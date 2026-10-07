/**
 * @file b8_v2_subscription_broadcast_pressure.cc
 * @brief Real SSE subscription broadcast pressure benchmark.
 */

#include "../common/benchmark_environment.h"

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

uint16_t pick_port()
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
    if (!galay::benchmark::initialize_benchmark_environment()) {
        return 1;
    }

    std::size_t iterations = 200'000;
    if (argc > 1) {
        const std::string_view text(argv[1]);
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), iterations);
        if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || iterations == 0) {
            std::cerr << "usage: benchmark_mcp_v2_subscription_broadcast_pressure [positive-iterations]\n";
            return 2;
        }
    }

    const auto port = pick_port();
    if (port == 0) {
        std::cerr << "failed to pick benchmark port\n";
        return 1;
    }

    galay::mcp::v2::McpHttpServer server("127.0.0.1", port, 2, 0);
    server.add_tool("pressure", "pressure", "{}",
                   [](const json::Json&,
                      std::expected<std::string, galay::mcp::McpError>& result)
                       -> galay::kernel::Task<void> {
                       result = "ok";
                       co_return;
                   });
    server.add_resource("mem://pressure", "pressure", "pressure", "text/plain",
                       [](const std::string&,
                          std::expected<std::string, galay::mcp::McpError>& result)
                           -> galay::kernel::Task<void> {
                           result = "ok";
                           co_return;
                       });
    server.add_prompt("pressure", "pressure", {},
                     [](const std::string&, const json::Json&,
                        std::expected<std::string, galay::mcp::McpError>& result)
                         -> galay::kernel::Task<void> {
                         result = R"({"messages":[]})";
                         co_return;
                     });

    std::thread serverThread([&server] { server.start(); });
    const auto serverDeadline = std::chrono::steady_clock::now() + 3s;
    while (!server.is_running() && std::chrono::steady_clock::now() < serverDeadline) {
        std::this_thread::sleep_for(1ms);
    }
    if (!server.is_running()) {
        server.stop();
        serverThread.join();
        std::cerr << "benchmark server did not start\n";
        return 1;
    }

    galay::kernel::Runtime runtime =
        galay::kernel::RuntimeBuilder().io_scheduler_count(1).parallel_scheduler_count(0).build();
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
        std::unexpected(galay::mcp::McpError::invalid_response("pending acknowledgement"));
    std::atomic<std::size_t> received{0};
    std::atomic<bool> subscribed{false};
    std::atomic<bool> warmed{false};
    auto listener = client.listen(
        filter,
        [&received, &subscribed, &warmed](std::string message) {
            subscribed.store(true, std::memory_order_release);
            if (warmed.load(std::memory_order_acquire)) {
                received.fetch_add(1, std::memory_order_release);
            } else if (message.find("notifications/resources/list_changed") != std::string::npos) {
                warmed.store(true, std::memory_order_release);
            }
            return true;
        },
        listenResult);
    auto listenerHandle = runtime.spawn_io(std::move(listener));
    if (!listenerHandle) {
        server.stop();
        serverThread.join();
        runtime.stop();
        std::cerr << "failed to schedule benchmark subscription\n";
        return 1;
    }

    const auto acknowledgementDeadline = std::chrono::steady_clock::now() + 3s;
    bool warmupEnqueued = false;
    while (!subscribed.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < acknowledgementDeadline) {
        warmupEnqueued = server.notify_tools_list_changed().has_value();
        if (!warmupEnqueued) break;
        std::this_thread::sleep_for(1ms);
    }
    // All warmup commands and this marker share one producer's FIFO stream.
    // Receiving the marker proves warmup drained, without a timing assumption.
    const auto marker = server.notify_resources_list_changed();
    while (marker && !warmed.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < acknowledgementDeadline) {
        std::this_thread::sleep_for(50us);
    }
    if (!warmupEnqueued || !marker || !warmed.load(std::memory_order_acquire)) {
        server.stop();
        const auto joined = listenerHandle->join();
        serverThread.join();
        runtime.stop();
        if (!joined) std::cerr << "subscription join failed" << std::endl;
        std::cerr << "subscription acknowledgement timed out\n";
        return 1;
    }

    const std::size_t publishCalls = iterations * 4;
    const auto start = std::chrono::steady_clock::now();
    std::size_t enqueued = 0;
    for (std::size_t i = 0; i < iterations; ++i) {
        enqueued += server.notify_tools_list_changed().has_value();
        enqueued += server.notify_resources_list_changed().has_value();
        enqueued += server.notify_prompts_list_changed().has_value();
        enqueued += server.notify_resource_updated("mem://pressure").has_value();
        // Bound in-flight events below the subscriber's capacity. Command
        // acceptance and actual delivery are deliberately checked separately.
        if ((i + 1) % 32 == 0) {
            const auto drainDeadline = std::chrono::steady_clock::now() + 5s;
            while (received.load(std::memory_order_acquire) < enqueued &&
                   std::chrono::steady_clock::now() < drainDeadline) {
                std::this_thread::sleep_for(50us);
            }
            if (received.load(std::memory_order_acquire) < enqueued) break;
        }
    }
    const auto listenerDeadline = std::chrono::steady_clock::now() + 5s;
    while (received.load(std::memory_order_acquire) < enqueued &&
           std::chrono::steady_clock::now() < listenerDeadline) {
        std::this_thread::sleep_for(1ms);
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - start).count();
    const auto callbackEvents = received.load(std::memory_order_acquire);
    server.stop();
    const auto joined = listenerHandle->join();
    serverThread.join();
    runtime.stop();

    if (!joined || elapsed <= 0 || enqueued != publishCalls || callbackEvents != enqueued || !listenResult.has_value()) return 1;
    std::cout << "MCP v2 subscription iterations: " << iterations << '\n'
              << "Broadcast calls: " << publishCalls << '\n'
              << "Enqueued events: " << enqueued << '\n'
              << "Callback events: " << callbackEvents << '\n'
              << "End-to-end throughput (including drain waits): "
              << (static_cast<double>(publishCalls) * 1'000'000'000.0 / elapsed)
              << " events/s\n"
              << "Amortized end-to-end time: "
              << (static_cast<double>(elapsed) / publishCalls)
              << " ns/event\n";
    return 0;
}
