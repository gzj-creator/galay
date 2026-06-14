/**
 * @file t3_tcp.cc
 * @brief 用途：验证 TCP Echo Server 测试路径能够正确监听、收包并回显。
 * 关键覆盖点：监听与接受连接、请求回显闭环、回包内容与字节数一致性。
 * 通过条件：Echo 服务端完成一轮有效回显，相关断言成立并返回 0。
 */

#include <iostream>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <string_view>
#include <thread>
#include "kernel/async/tcp_socket.h"
#include "kernel/kernel/task.h"
#include "test/port_cfg.h"
#include "test/stdout_log.h"
#include "result_writer.h"

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
using namespace std::chrono_literals;

namespace {

uint16_t tcpTestPort() {
    return galay::test::resolvePortFromEnv("GALAY_TEST_TCP_PORT", 8080);
}

constexpr std::array<std::string_view, 3> kMessages{
    "Hello, Server!",
    "This is message 2",
    "Final message",
};

bool waitForFlag(const std::atomic<bool>& flag, std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (flag.load(std::memory_order_acquire)) {
            return true;
        }
        std::this_thread::sleep_for(5ms);
    }
    return flag.load(std::memory_order_acquire);
}

}

std::atomic<int> g_passed{0};
std::atomic<int> g_failed{0};
std::atomic<int> g_total{0};
std::atomic<bool> g_server_ready{false};
std::atomic<bool> g_test_done{false};

// Echo服务器协程
Task<void> echoServer() {
    g_total++;
    LogInfo("TCP Server starting...");
    TcpSocket listener;

    // 设置选项
    auto optResult = listener.option().handleReuseAddr();
    if (!optResult) {
        LogError("Failed to set reuse addr: {}", optResult.error().message());
        g_failed++;
        g_test_done = true;
        co_return;
    }

    optResult = listener.option().handleNonBlock();
    if (!optResult) {
        LogError("Failed to set non-block: {}", optResult.error().message());
        g_failed++;
        g_test_done = true;
        co_return;
    }

    // 绑定地址
    Host bindHost(IPType::IPV4, "127.0.0.1", tcpTestPort());
    auto bindResult = listener.bind(bindHost);
    if (!bindResult) {
        LogError("Failed to bind: {}", bindResult.error().message());
        g_failed++;
        g_test_done = true;
        co_return;
    }
    LogDebug("Bind successful");

    // 监听
    auto listenResult = listener.listen(128);
    if (!listenResult) {
        LogError("Failed to listen: {}", listenResult.error().message());
        g_failed++;
        g_test_done = true;
        co_return;
    }

    LogInfo("TCP Server listening on 127.0.0.1:{}", tcpTestPort());
    g_server_ready = true;

    // 接受连接
    Host clientHost;
    LogDebug("Waiting for accept...");
    auto acceptResult = co_await listener.accept(&clientHost);
    if (!acceptResult) {
        LogError("Failed to accept: {}", acceptResult.error().message());
        g_failed++;
        g_test_done = true;
        co_return;
    }

    LogInfo("Client connected from {}:{}", clientHost.ip(), clientHost.port());

    // 创建客户端socket
    TcpSocket client(acceptResult.value());
    client.option().handleNonBlock();

    // Echo循环 - 接收3条消息
    char buffer[1024];
    int message_count = 0;
    while (message_count < 3) {
        LogDebug("Waiting for recv...");
        auto recvResult = co_await client.recv(buffer, sizeof(buffer));
        if (!recvResult) {
            LogError("Recv error: {}", recvResult.error().message());
            g_failed++;
            break;
        }

        size_t bytes = recvResult.value();
        if (bytes == 0) {
            LogInfo("Client disconnected");
            break;
        }

        LogInfo("Received: {}", std::string_view(buffer, bytes));

        auto sendResult = co_await client.send(buffer, bytes);
        if (!sendResult) {
            LogError("Send error: {}", sendResult.error().message());
            g_failed++;
            break;
        }
        LogDebug("Sent {} bytes", sendResult.value());
        message_count++;
    }

    if (message_count == 3) {
        LogInfo("Test PASSED: Received and echoed 3 messages");
        g_passed++;
    } else {
        LogError("Test FAILED: Only received {} messages", message_count);
        g_failed++;
    }

    co_await client.close();
    co_await listener.close();
    LogInfo("TCP Server stopped");
    g_test_done = true;
    co_return;
}

Task<void> peerEchoClient() {
    TcpSocket client;
    client.option().handleNonBlock();

    Host serverHost(IPType::IPV4, "127.0.0.1", tcpTestPort());
    auto connectResult = co_await client.connect(serverHost);
    if (!connectResult) {
        LogError("Peer client failed to connect: {}", connectResult.error().message());
        g_failed++;
        g_test_done = true;
        co_return;
    }

    char buffer[1024];
    for (const auto message : kMessages) {
        auto sendResult = co_await client.send(message.data(), message.size());
        if (!sendResult) {
            LogError("Peer client send failed: {}", sendResult.error().message());
            g_failed++;
            g_test_done = true;
            co_return;
        }

        auto recvResult = co_await client.recv(buffer, sizeof(buffer));
        if (!recvResult || std::string_view(buffer, recvResult.value()) != message) {
            LogError("Peer client echo verification failed");
            g_failed++;
            g_test_done = true;
            co_return;
        }
    }

    co_await client.close();
    co_return;
}

int main() {
    LogInfo("========================================");
    LogInfo("TCP Echo Server Test");
    LogInfo("========================================\n");

    galay::test::TestResultWriter writer("test_tcp_server");

#if defined(USE_KQUEUE) || defined(USE_EPOLL) || defined(USE_IOURING)
    IOSchedulerType scheduler;
    scheduler.start();
    LogDebug("Scheduler started");

    // 启动服务器
    scheduleTask(scheduler, echoServer());
    LogDebug("Server task submitted");

    if (!waitForFlag(g_server_ready, 5s) && !g_test_done.load()) {
        LogError("Server did not become ready in time");
        g_failed++;
        g_test_done = true;
    }

    if (!g_test_done.load()) {
        LogInfo("Server is ready, scheduling async peer client...");
        if (!scheduleTask(scheduler, peerEchoClient())) {
            LogError("Failed to schedule async peer client");
            g_failed++;
            g_test_done = true;
        }
    }

    if (!waitForFlag(g_test_done, 5s)) {
        LogError("TCP server test timed out waiting for completion");
        g_failed++;
    }

    scheduler.stop();
    LogInfo("Test finished");
#else
    LogWarn("This test requires kqueue (macOS), epoll or io_uring (Linux)");
    g_failed++;
#endif

    // 写入测试结果
    writer.addTest();
    if (g_passed > 0) {
        writer.addPassed();
    }
    if (g_failed > 0) {
        writer.addFailed();
    }
    writer.writeResult();

    LogInfo("========================================");
    LogInfo("Test Results: Total={}, Passed={}, Failed={}", g_total.load(), g_passed.load(), g_failed.load());
    LogInfo("========================================");

    return g_failed > 0 ? 1 : 0;
}
