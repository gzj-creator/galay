/**
 * @file e3_tcp.cc
 * @brief 用途：用模块导入方式演示 TCP 客户端主动连接与收发流程。
 * 关键覆盖点：模块导入、客户端建连、请求发送与响应接收。
 * 通过条件：请求响应闭环跑通并返回 0。
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
constexpr uint16_t kPort = 9083;
std::atomic<bool> g_server_ready{false};
std::atomic<bool> g_server_done{false};
std::atomic<bool> g_client_done{false};

Task<void> tinyServer() {
    TcpSocket listener;

    auto optResult = listener.option().handleReuseAddr();
    if (!optResult) {
        g_server_done.store(true, std::memory_order_release);
        co_return;
    }

    optResult = listener.option().handleNonBlock();
    if (!optResult) {
        g_server_done.store(true, std::memory_order_release);
        co_return;
    }

    auto bindResult = listener.bind(Host(IPType::IPV4, "127.0.0.1", kPort));
    if (!bindResult) {
        g_server_done.store(true, std::memory_order_release);
        co_return;
    }

    auto listenResult = listener.listen(16);
    if (!listenResult) {
        g_server_done.store(true, std::memory_order_release);
        co_return;
    }

    g_server_ready.store(true, std::memory_order_release);

    Host peer;
    auto accepted = co_await listener.accept(&peer);
    if (!accepted) {
        co_await listener.close();
        g_server_done.store(true, std::memory_order_release);
        co_return;
    }

    TcpSocket client(accepted.value());
    optResult = client.option().handleNonBlock();
    if (!optResult) {
        co_await client.close();
        co_await listener.close();
        g_server_done.store(true, std::memory_order_release);
        co_return;
    }

    char buffer[256];
    auto recvResult = co_await client.recv(buffer, sizeof(buffer));
    if (recvResult && recvResult.value() > 0) {
        constexpr const char* kResponse = "pong from import server";
        auto sendResult = co_await client.send(kResponse, std::char_traits<char>::length(kResponse));
        if (!sendResult) {
            std::cerr << "server send failed\n";
        }
    }

    co_await client.close();
    co_await listener.close();
    g_server_done.store(true, std::memory_order_release);
}

Task<void> tcpClient() {
    while (!g_server_ready.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    TcpSocket socket;
    auto optResult = socket.option().handleNonBlock();
    if (!optResult) {
        g_client_done.store(true, std::memory_order_release);
        co_return;
    }

    auto connectResult = co_await socket.connect(Host(IPType::IPV4, "127.0.0.1", kPort));
    if (!connectResult) {
        g_client_done.store(true, std::memory_order_release);
        co_return;
    }

    std::string message = "ping from import client";
    auto sendResult = co_await socket.send(message.c_str(), message.size());
    if (!sendResult) {
        co_await socket.close();
        g_client_done.store(true, std::memory_order_release);
        co_return;
    }

    char buffer[256];
    auto recvResult = co_await socket.recv(buffer, sizeof(buffer));
    if (recvResult && recvResult.value() > 0) {
        std::cout << "tcp client received: " << std::string_view(buffer, recvResult.value()) << "\n";
    }

    co_await socket.close();
    g_client_done.store(true, std::memory_order_release);
}
}  // namespace

int main() {
    Runtime runtime = RuntimeBuilder().ioSchedulerCount(1).computeSchedulerCount(1).build();
    runtime.start();

    auto* io = runtime.getNextIOScheduler();
    scheduleTask(io, tinyServer());
    scheduleTask(io, tcpClient());

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while ((!g_server_done.load(std::memory_order_acquire) ||
            !g_client_done.load(std::memory_order_acquire)) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    runtime.stop();
    return 0;
}
