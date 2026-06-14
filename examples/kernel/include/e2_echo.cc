/**
 * @file e2_echo.cc
 * @brief 用途：用头文件方式演示 TCP Echo 自闭环服务端路径。
 * 关键覆盖点：监听与接受连接、回显处理、客户端侧闭环验证。
 * 通过条件：完成至少一轮成功回显并返回 0。
 */

#include <atomic>
#include <chrono>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>

#include "kernel/async/tcp_socket.h"
#include "kernel/kernel/task.h"

#ifdef USE_KQUEUE
#include "kernel/kernel/kqueue_scheduler.h"
using IOSchedulerType = galay::kernel::KqueueScheduler;
#endif

#ifdef USE_EPOLL
#include "kernel/kernel/epoll_scheduler.h"
using IOSchedulerType = galay::kernel::EpollScheduler;
#endif

#ifdef USE_IOURING
#include "kernel/kernel/uring_scheduler.h"
using IOSchedulerType = galay::kernel::IOUringScheduler;
#endif

using namespace galay::async;
using namespace galay::kernel;

namespace {
constexpr uint16_t kPort = 9082;
std::atomic<bool> g_server_ready{false};
std::atomic<bool> g_done{false};
std::atomic<bool> g_ok{false};

Task<void> echoServer() {
    TcpSocket listener;
    listener.option().handleReuseAddr();
    listener.option().handleNonBlock();

    if (!listener.bind(Host(IPType::IPV4, "127.0.0.1", kPort))) {
        g_done.store(true, std::memory_order_release);
        co_return;
    }
    if (!listener.listen(16)) {
        g_done.store(true, std::memory_order_release);
        co_return;
    }

    g_server_ready.store(true, std::memory_order_release);

    Host peer;
    auto accepted = co_await listener.accept(&peer);
    if (!accepted) {
        co_await listener.close();
        g_done.store(true, std::memory_order_release);
        co_return;
    }

    TcpSocket client(accepted.value());
    client.option().handleNonBlock();

    char buffer[256]{};
    auto recvResult = co_await client.recv(buffer, sizeof(buffer));
    if (recvResult && recvResult.value() > 0) {
        auto sendResult = co_await client.send(buffer, recvResult.value());
        g_ok.store(static_cast<bool>(sendResult), std::memory_order_release);
    }

    co_await client.close();
    co_await listener.close();
    g_done.store(true, std::memory_order_release);
}

Task<void> echoClient() {
    while (!g_server_ready.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    TcpSocket socket;
    socket.option().handleNonBlock();

    auto connected = co_await socket.connect(Host(IPType::IPV4, "127.0.0.1", kPort));
    if (!connected) {
        co_return;
    }

    constexpr const char* kMsg = "hello from E2";
    auto sendResult = co_await socket.send(kMsg, std::char_traits<char>::length(kMsg));
    if (!sendResult) {
        co_await socket.close();
        co_return;
    }

    char buffer[256]{};
    auto recvResult = co_await socket.recv(buffer, sizeof(buffer));
    if (recvResult && recvResult.value() > 0) {
        g_ok.store(std::string_view(buffer, recvResult.value()) == kMsg, std::memory_order_release);
    }

    co_await socket.close();
}
}  // namespace

int main() {
    IOSchedulerType scheduler;
    scheduler.start();

    scheduleTask(scheduler, echoServer());
    scheduleTask(scheduler, echoClient());

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!g_done.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    scheduler.stop();

    if (!g_ok.load(std::memory_order_acquire)) {
        std::cerr << "tcp echo server example failed\n";
        return 1;
    }

    std::cout << "tcp echo server example passed\n";
    return 0;
}
