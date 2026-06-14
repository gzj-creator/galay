/**
 * @file t7_udp.cc
 * @brief 用途：验证 UDP Echo Client 测试路径能够发送报文并收到正确回显。
 * 关键覆盖点：客户端发包、接收回包、回显内容与来源地址校验。
 * 通过条件：客户端收到预期回包，测试断言全部成立并返回 0。
 */

#include <atomic>
#include <chrono>
#include <cstring>
#include <string_view>
#include <thread>
#include "kernel/async/udp_socket.h"
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

uint16_t udpTestPort() {
    return galay::test::resolvePortFromEnv("GALAY_TEST_UDP_PORT", 8080);
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

// UDP客户端协程
Task<void> udpEchoClient() {
    g_total++;
    LogInfo("UDP Client starting...");
    UdpSocket socket;
    LogDebug("Client socket created, fd={}", socket.handle().fd);

    socket.option().handleNonBlock();

    // 服务器地址
    Host serverHost(IPType::IPV4, "127.0.0.1", udpTestPort());

    // 发送3条消息并验证回显
    const char* messages[] = {
        "Hello, UDP Server!",
        "This is message 2",
        "Final message"
    };

    int success_count = 0;
    for (int i = 0; i < 3; ++i) {
        // 发送消息
        LogDebug("Client sending message {}...", i + 1);
        auto sendResult = co_await socket.sendto(messages[i], strlen(messages[i]), serverHost);
        if (!sendResult) {
            LogError("Client: Sendto failed for message {}", i + 1);
            g_failed++;
            continue;
        }

        LogInfo("Client: Sent message {}: {}", i + 1, messages[i]);

        // 接收回复
        char buffer[1024];
        Host from;
        auto recvResult = co_await socket.recvfrom(buffer, sizeof(buffer), &from);
        if (!recvResult) {
            LogError("Client: Recvfrom failed for message {}", i + 1);
            g_failed++;
            continue;
        }

        size_t bytes = recvResult.value();
        auto payload = std::string_view(buffer, bytes);
        LogInfo("Client: Received echo from {}:{}: {}", from.ip(), from.port(), payload);

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

    co_await socket.close();
    LogInfo("UDP Client stopped");
    g_test_done = true;
    co_return;
}

Task<void> peerUdpServer() {
    UdpSocket socket;
    socket.option().handleReuseAddr();
    socket.option().handleNonBlock();

    Host bindHost(IPType::IPV4, "127.0.0.1", udpTestPort());
    auto bindResult = socket.bind(bindHost);
    if (!bindResult) {
        LogError("Peer UDP server failed to bind");
        g_failed++;
        g_test_done = true;
        co_return;
    }

    g_server_ready = true;

    char buffer[1024];
    for (int i = 0; i < 3; ++i) {
        Host from;
        auto recvResult = co_await socket.recvfrom(buffer, sizeof(buffer), &from);
        if (!recvResult) {
            LogError("Peer UDP server recv failed");
            g_failed++;
            g_test_done = true;
            co_return;
        }

        auto sendResult = co_await socket.sendto(buffer, recvResult.value(), from);
        if (!sendResult) {
            LogError("Peer UDP server send failed");
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
    LogInfo("UDP Echo Client Test");
    LogInfo("========================================\n");

    galay::test::TestResultWriter writer("test_udp_client");

#if defined(USE_KQUEUE) || defined(USE_EPOLL) || defined(USE_IOURING)
    IOSchedulerType scheduler;
    scheduler.start();
    LogDebug("Scheduler started");

    if (!scheduleTask(scheduler, peerUdpServer())) {
        LogError("Failed to schedule async peer UDP server");
        g_failed++;
        g_test_done = true;
    }

    if (!waitForFlag(g_server_ready, 5s) && !g_test_done.load()) {
        LogError("Peer UDP server did not become ready in time");
        g_failed++;
        g_test_done = true;
    }

    // 启动客户端
    if (!g_test_done.load()) {
        scheduleTask(scheduler, udpEchoClient());
        LogDebug("Client task submitted");
    }

    if (!waitForFlag(g_test_done, 5s)) {
        LogError("UDP client test timed out waiting for completion");
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
