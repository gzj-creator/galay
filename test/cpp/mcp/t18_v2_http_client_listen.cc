/**
 * @file t18_v2_http_client_listen.cc
 * @brief v2 HTTP client SSE listen lifecycle and independent request coverage.
 */

#include <galay/cpp/galay-kernel/common/sleep.hpp>
#include <galay/cpp/galay-kernel/core/runtime.h>
#include <galay/cpp/galay-mcp/v2/client/client.h>
#include <galay/cpp/galay-mcp/v2/server/http_server.h>

#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <netinet/in.h>
#include <string>
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
    const auto port = static_cast<uint16_t>(ntohs(address.sin_port));
    ::close(fd);
    return ok ? port : 0;
}

galay::kernel::Task<void> runListener(
    galay::mcp::v2::McpHttpClient* client,
    galay::mcp::v2::SubscriptionFilter filter,
    std::expected<galay::mcp::v2::SubscriptionFilter, galay::mcp::McpError>* listenResult,
    std::atomic<int>* callbacks,
    std::atomic<bool>* callbackOwned,
    std::atomic<bool>* listenerDone)
{
    co_await client->listen(
        std::move(filter),
        [callbacks, callbackOwned](std::string message) {
            callbackOwned->store(!message.empty(), std::memory_order_release);
            const auto count = callbacks->fetch_add(1, std::memory_order_acq_rel) + 1;
            return count < 2;
        },
        *listenResult);
    listenerDone->store(true, std::memory_order_release);
}

galay::kernel::Task<void> runClientChecks(
    galay::kernel::Runtime* runtime,
    galay::mcp::v2::McpHttpClient* client,
    std::expected<galay::mcp::v2::SubscriptionFilter, galay::mcp::McpError>* listenResult,
    std::atomic<int>* callbacks,
    std::atomic<bool>* callbackOwned,
    std::atomic<bool>* discoverOk,
    std::atomic<bool>* listenerDone,
    std::atomic<bool>* taskDone)
{
    galay::mcp::v2::SubscriptionFilter filter;
    filter.toolsListChanged = true;
    auto listenTask = runListener(client, std::move(filter), listenResult,
                                  callbacks, callbackOwned, listenerDone);

    auto listenHandle = runtime->spawnIO(std::move(listenTask));
    if (!listenHandle) co_return;

    const auto firstEventDeadline = std::chrono::steady_clock::now() + 3s;
    while (callbacks->load(std::memory_order_acquire) == 0 &&
           std::chrono::steady_clock::now() < firstEventDeadline) {
        co_await galay::kernel::sleep(2ms);
    }
    if (callbacks->load(std::memory_order_acquire) == 0) {
        taskDone->store(true, std::memory_order_release);
        co_return;
    }

    std::expected<galay::mcp::v2::DiscoverResult, galay::mcp::McpError> discover;
    co_await client->discover(discover);
    if (!discover) {
        taskDone->store(true, std::memory_order_release);
        co_return;
    }
    discoverOk->store(true, std::memory_order_release);

    const auto callbackDeadline = std::chrono::steady_clock::now() + 3s;
    while (callbacks->load(std::memory_order_acquire) < 2 &&
           std::chrono::steady_clock::now() < callbackDeadline) {
        co_await galay::kernel::sleep(2ms);
    }
    while (!listenerDone->load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < callbackDeadline) {
        co_await galay::kernel::sleep(2ms);
    }
    taskDone->store(true, std::memory_order_release);
    co_return;
}

} // namespace

int main()
{
    const auto port = pickPort();
    if (port == 0) {
        std::cerr << "failed to pick v2 client test port\n";
        return 1;
    }

    galay::mcp::v2::McpHttpServer server("127.0.0.1", port, 1, 1);
    server.addTool("echo", "Echo", "{}",
                   [](const galay::mcp::JsonElement&,
                      std::expected<galay::mcp::JsonString, galay::mcp::McpError>& result)
                       -> galay::kernel::Task<void> {
                       result = "ok";
                       co_return;
                   });
    std::thread serverThread([&server] { server.start(); });
    std::this_thread::sleep_for(80ms);

    galay::kernel::Runtime runtime =
        galay::kernel::RuntimeBuilder().ioSchedulerCount(1).computeSchedulerCount(0).build();
    runtime.start();
    galay::mcp::v2::McpHttpClient client(
        runtime, "http://127.0.0.1:" + std::to_string(port) + "/mcp");

    std::expected<galay::mcp::v2::SubscriptionFilter, galay::mcp::McpError> listenResult =
        std::unexpected(galay::mcp::McpError::invalidResponse("pending acknowledgement"));
    std::atomic<int> callbacks{0};
    std::atomic<bool> callbackOwned{false};
    std::atomic<bool> discoverOk{false};
    std::atomic<bool> listenerDone{false};
    std::atomic<bool> taskDone{false};

    auto handle = runtime.spawnIO(runClientChecks(
        &runtime, &client, &listenResult, &callbacks, &callbackOwned,
        &discoverOk, &listenerDone, &taskDone));
    if (!handle) {
        std::cerr << "failed to schedule v2 client checks\n";
        server.stop();
        serverThread.join();
        runtime.stop();
        return 1;
    }

    const auto deadline = std::chrono::steady_clock::now() + 7s;
    while (!taskDone.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        server.notifyToolsListChanged();
        std::this_thread::sleep_for(10ms);
    }
    const bool ok = listenResult.has_value() &&
                    listenResult->toolsListChanged &&
                    !listenResult->resourcesListChanged &&
                    callbacks.load() == 2 &&
                    callbackOwned.load(std::memory_order_acquire) &&
                    discoverOk.load(std::memory_order_acquire) &&
                    listenerDone.load(std::memory_order_acquire);
    if (!ok) {
        std::cerr << "listen=" << listenResult.has_value()
                  << " callbacks=" << callbacks.load()
                  << " discover=" << discoverOk.load()
                  << " listener_done=" << listenerDone.load()
                  << " done=" << taskDone.load();
        if (!listenResult.has_value()) {
            std::cerr << " error=" << listenResult.error().message()
                      << " details=" << listenResult.error().details();
        }
        std::cerr << '\n';
    }
    (void)handle->join();
    (void)runtime.blockOnIO(client.close());
    runtime.stop();
    server.stop();
    serverThread.join();

    if (!ok) {
        std::cerr << "v2 client listen did not acknowledge, callback, and cancel cleanly\n";
        return 1;
    }
    std::cout << "T18-V2HttpClientListen PASS\n";
    return 0;
}
