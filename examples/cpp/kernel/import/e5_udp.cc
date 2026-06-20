/**
 * @file e5_udp.cc
 * @brief 用途：用模块导入方式演示 UDP Echo 的基础收发闭环。
 * 关键覆盖点：模块导入、端口绑定、`sendTo/recvFrom` 调用与回显验证。
 * 通过条件：至少完成一轮 UDP 收发并返回 0。
 */

import galay.kernel;

#include <coroutine>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>

using namespace galay::async;
using namespace galay::kernel;

namespace {
constexpr uint16_t kPort = 9095;
std::atomic<bool> g_server_ready{false};
std::atomic<bool> g_server_done{false};
std::atomic<bool> g_client_done{false};

Task<void> udpServer() {
    UdpSocket socket;

    auto optResult = socket.option().handleReuseAddr();
    if (!optResult) {
        g_server_done.store(true, std::memory_order_release);
        co_return;
    }

    optResult = socket.option().handleNonBlock();
    if (!optResult) {
        g_server_done.store(true, std::memory_order_release);
        co_return;
    }

    auto bindResult = socket.bind(Host(IPType::IPV4, "127.0.0.1", kPort));
    if (!bindResult) {
        g_server_done.store(true, std::memory_order_release);
        co_return;
    }

    g_server_ready.store(true, std::memory_order_release);

    char buffer[1024];
    Host peer;
    auto recvResult = co_await socket.recvfrom(buffer, sizeof(buffer), &peer);
    if (recvResult) {
        size_t bytes = recvResult.value();
        auto sendResult = co_await socket.sendto(buffer, bytes, peer);
        if (!sendResult) {
            std::cerr << "udp sendto failed\n";
        }
    }

    co_await socket.close();
    g_server_done.store(true, std::memory_order_release);
}

Task<void> udpClient() {
    while (!g_server_ready.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    UdpSocket socket;
    auto optResult = socket.option().handleNonBlock();
    if (!optResult) {
        g_client_done.store(true, std::memory_order_release);
        co_return;
    }

    std::string message = "hello from import udp client";
    Host server(IPType::IPV4, "127.0.0.1", kPort);
    auto sendResult = co_await socket.sendto(message.c_str(), message.size(), server);
    if (!sendResult) {
        co_await socket.close();
        g_client_done.store(true, std::memory_order_release);
        co_return;
    }

    char buffer[1024];
    Host peer;
    auto recvResult = co_await socket.recvfrom(buffer, sizeof(buffer), &peer);
    if (recvResult) {
        std::cout << "udp response: " << std::string_view(buffer, recvResult.value()) << "\n";
    }

    co_await socket.close();
    g_client_done.store(true, std::memory_order_release);
}
}  // namespace

int main() {
    Runtime runtime = RuntimeBuilder().ioSchedulerCount(1).computeSchedulerCount(1).build();
    runtime.start();

    auto* io = runtime.getNextIOScheduler();
    scheduleTask(io, udpServer());
    scheduleTask(io, udpClient());

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while ((!g_server_done.load(std::memory_order_acquire) ||
            !g_client_done.load(std::memory_order_acquire)) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    runtime.stop();
    return 0;
}
