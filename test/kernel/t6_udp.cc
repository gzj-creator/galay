/**
 * @file t6_udp.cc
 * @brief 用途：验证 UDP Echo Server 测试路径能够接收报文并正确回发。
 * 关键覆盖点：端口绑定、报文接收与回显、载荷内容保持一致。
 * 通过条件：服务端完成有效回显，断言成立并返回 0。
 */

#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <string_view>
#include <thread>
#include "galay-kernel/async/udp_socket.h"
#include "galay-kernel/core/task.h"
#include "test/port_cfg.h"
#include "test/stdout_log.h"
#include "result_writer.h"

#ifdef USE_KQUEUE
#include "galay-kernel/core/kqueue_scheduler.h"
using IOSchedulerType = galay::kernel::KqueueScheduler;
#endif

#ifdef USE_EPOLL
#include "galay-kernel/core/epoll_scheduler.h"
using IOSchedulerType = galay::kernel::EpollScheduler;
#endif

#ifdef USE_IOURING
#include "galay-kernel/core/uring_scheduler.h"
using IOSchedulerType = galay::kernel::IOUringScheduler;
#endif

using namespace galay::async;
using namespace galay::kernel;
using namespace std::chrono_literals;

namespace {

uint16_t udpTestPort() {
    return galay::test::resolvePortFromEnv("GALAY_TEST_UDP_PORT", 8080);
}

constexpr std::array<std::string_view, 3> kMessages{
    "Hello, UDP Server!",
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

// UDP Echo服务器协程
Task<void> udpEchoServer() {
    g_total++;
    LogInfo("UDP Server starting...");
    UdpSocket socket;
    LogDebug("Socket created, fd={}", socket.handle().fd);

    // 设置选项
    auto optResult = socket.option().handleReuseAddr();
    if (!optResult) {
        LogError("Failed to set reuse addr: {}", optResult.error().message());
        g_failed++;
        g_test_done = true;
        co_return;
    }

    optResult = socket.option().handleNonBlock();
    if (!optResult) {
        LogError("Failed to set non-block: {}", optResult.error().message());
        g_failed++;
        g_test_done = true;
        co_return;
    }

    // 绑定地址
    Host bindHost(IPType::IPV4, "127.0.0.1", udpTestPort());
    auto bindResult = socket.bind(bindHost);
    if (!bindResult) {
        LogError("Failed to bind: {}", bindResult.error().message());
        g_failed++;
        g_test_done = true;
        co_return;
    }
    LogDebug("Bind successful");

    LogInfo("UDP Server listening on 127.0.0.1:{}", udpTestPort());
    g_server_ready = true;

    // Echo循环 - 接收并回显3个数据报
    char buffer[65536];
    int message_count = 0;
    for (int i = 0; i < 3; ++i) {
        Host from;
        LogDebug("Waiting for recvfrom...");
        auto recvResult = co_await socket.recvfrom(buffer, sizeof(buffer), &from);
        if (!recvResult) {
            LogError("Recvfrom error: {}", recvResult.error().message());
            g_failed++;
            break;
        }

        size_t bytes = recvResult.value();
        LogInfo("Received from {}:{}: {}", from.ip(), from.port(), std::string_view(buffer, bytes));

        // Echo回发送方
        auto sendResult = co_await socket.sendto(buffer, bytes, from);
        if (!sendResult) {
            LogError("Sendto error: {}", sendResult.error().message());
            g_failed++;
            break;
        }
        LogDebug("Sent {} bytes back to {}:{}", sendResult.value(), from.ip(), from.port());
        message_count++;
    }

    if (message_count == 3) {
        LogInfo("Test PASSED: Received and echoed 3 messages");
        g_passed++;
    } else {
        LogError("Test FAILED: Only received {} messages", message_count);
        g_failed++;
    }

    co_await socket.close();
    LogInfo("UDP Server stopped");
    g_test_done = true;
    co_return;
}

Task<void> peerUdpClient() {
    UdpSocket socket;
    socket.option().handleNonBlock();

    Host serverHost(IPType::IPV4, "127.0.0.1", udpTestPort());
    char buffer[1024];

    for (const auto message : kMessages) {
        auto sendResult = co_await socket.sendto(message.data(), message.size(), serverHost);
        if (!sendResult) {
            LogError("Peer UDP client send failed");
            g_failed++;
            g_test_done = true;
            co_return;
        }

        Host from;
        auto recvResult = co_await socket.recvfrom(buffer, sizeof(buffer), &from);
        if (!recvResult || std::string_view(buffer, recvResult.value()) != message) {
            LogError("Peer UDP client echo verification failed");
            g_failed++;
            g_test_done = true;
            co_return;
        }
    }

    co_await socket.close();
    co_return;
}

int main() {
    LogInfo("========================================");
    LogInfo("UDP Echo Server Test");
    LogInfo("========================================\n");

    galay::test::TestResultWriter writer("test_udp_server");

#if defined(USE_KQUEUE) || defined(USE_EPOLL) || defined(USE_IOURING)
    IOSchedulerType scheduler;
    scheduler.start();
    LogDebug("Scheduler started");

    // 启动服务器
    scheduleTask(scheduler, udpEchoServer());
    LogDebug("Server task submitted");

    if (!waitForFlag(g_server_ready, 5s) && !g_test_done.load()) {
        LogError("UDP server did not become ready in time");
        g_failed++;
        g_test_done = true;
    }

    if (!g_test_done.load()) {
        LogInfo("Server is ready, scheduling async peer UDP client...");
        if (!scheduleTask(scheduler, peerUdpClient())) {
            LogError("Failed to schedule async peer UDP client");
            g_failed++;
            g_test_done = true;
        }
    }

    if (!waitForFlag(g_test_done, 5s)) {
        LogError("UDP server test timed out waiting for completion");
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
