/**
 * @file t4_tcp.cc
 * @brief 用途：验证 TCP Echo Client 测试路径能够主动连接并完成请求响应闭环。
 * 关键覆盖点：客户端建连、发送请求、接收回显响应以及内容一致性校验。
 * 通过条件：客户端收到预期回显结果，断言成立并返回 0。
 */

#include <iostream>
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

// 客户端协程
Task<void> echoClient() {
    g_total++;
    LogInfo("TCP Client starting...");
    TcpSocket client;
    LogDebug("Client socket created, fd={}", client.handle().fd);

    client.option().handleNonBlock();

    // 连接服务器
    Host serverHost(IPType::IPV4, "127.0.0.1", tcpTestPort());
    LogDebug("Client connecting to server...");
    auto connectResult = co_await client.connect(serverHost);
    if (!connectResult) {
        LogError("Client: Failed to connect: {}", connectResult.error().message());
        g_failed++;
        g_test_done = true;
        co_return;
    }

    LogInfo("Client: Connected to server");

    // 发送3条消息并验证回显
    const char* messages[] = {
        "Hello, Server!",
        "This is message 2",
        "Final message"
    };

    int success_count = 0;
    for (int i = 0; i < 3; ++i) {
        // 发送消息
        auto sendResult = co_await client.send(messages[i], strlen(messages[i]));
        if (!sendResult) {
            LogError("Client: Send failed for message {}", i + 1);
            g_failed++;
            continue;
        }

        LogInfo("Client: Sent message {}: {}", i + 1, messages[i]);

        // 接收回复
        char buffer[1024];
        auto recvResult = co_await client.recv(buffer, sizeof(buffer));
        if (!recvResult) {
            LogError("Client: Recv failed for message {}", i + 1);
            g_failed++;
            continue;
        }

        size_t bytes = recvResult.value();
        std::string_view payload(buffer, bytes);
        LogInfo("Client: Received echo: {}", payload);

        // 验证回显内容
        if (payload == messages[i]) {
            LogInfo("Client: Message {} echo verified", i + 1);
            success_count++;
        } else {
            LogError("Client: Message {} echo mismatch", i + 1);
            g_failed++;
        }
    }

    if (success_count == 3) {
        LogInfo("Test PASSED: All 3 messages echoed correctly");
        g_passed++;
    } else {
        LogError("Test FAILED: Only {} messages echoed correctly", success_count);
        if (g_failed == 0) g_failed++;
    }

    co_await client.close();
    LogInfo("TCP Client stopped");
    g_test_done = true;
    co_return;
}

Task<void> peerEchoServer() {
    TcpSocket listener;
    listener.option().handleReuseAddr();
    listener.option().handleNonBlock();

    Host bindHost(IPType::IPV4, "127.0.0.1", tcpTestPort());
    auto bindResult = listener.bind(bindHost);
    if (!bindResult || !listener.listen(128)) {
        LogError("Peer server failed to listen");
        g_failed++;
        g_test_done = true;
        co_return;
    }

    g_server_ready = true;

    Host clientHost;
    auto acceptResult = co_await listener.accept(&clientHost);
    if (!acceptResult) {
        LogError("Peer server failed to accept: {}", acceptResult.error().message());
        g_failed++;
        g_test_done = true;
        co_return;
    }

    TcpSocket client(acceptResult.value());
    client.option().handleNonBlock();

    char buffer[1024];
    for (size_t i = 0; i < 3; ++i) {
        auto recvResult = co_await client.recv(buffer, sizeof(buffer));
        if (!recvResult) {
            LogError("Peer server recv failed: {}", recvResult.error().message());
            g_failed++;
            g_test_done = true;
            co_return;
        }

        auto sendResult = co_await client.send(buffer, recvResult.value());
        if (!sendResult) {
            LogError("Peer server send failed: {}", sendResult.error().message());
            g_failed++;
            g_test_done = true;
            co_return;
        }
    }

    co_await client.close();
    co_await listener.close();
    co_return;
}

int main() {
    LogInfo("========================================");
    LogInfo("TCP Echo Client Test");
    LogInfo("========================================\n");

    galay::test::TestResultWriter writer("test_tcp_client");

#if defined(USE_KQUEUE) || defined(USE_EPOLL) || defined(USE_IOURING)
    IOSchedulerType scheduler;
    scheduler.start();
    LogDebug("Scheduler started");

    if (!scheduleTask(scheduler, peerEchoServer())) {
        LogError("Failed to schedule async peer server");
        g_failed++;
        g_test_done = true;
    }

    if (!waitForFlag(g_server_ready, 5s) && !g_test_done.load()) {
        LogError("Peer server did not become ready in time");
        g_failed++;
        g_test_done = true;
    }

    // 启动客户端
    if (!g_test_done.load()) {
        scheduleTask(scheduler, echoClient());
        LogDebug("Client task submitted");
    }

    if (!waitForFlag(g_test_done, 5s)) {
        LogError("TCP client test timed out waiting for completion");
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
