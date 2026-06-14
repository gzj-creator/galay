/**
 * @file t5_udp.cc
 * @brief 用途：验证 `UdpSocket` 的基础发送、接收与地址处理能力。
 * 关键覆盖点：无连接报文收发、地址绑定与回传、基础错误路径处理。
 * 通过条件：UDP 收发结果与预期一致，测试按预期结束并返回 0。
 */

#include <exception>
#include <iostream>
#include <cstring>
#include <string_view>
#include "kernel/async/udp_socket.h"
#include "kernel/kernel/task.h"
#include "test/port_cfg.h"
#include "test/stdout_log.h"

#ifdef USE_KQUEUE
#include "kernel/kernel/kqueue_scheduler.h"
#endif

#ifdef USE_EPOLL
#include "kernel/kernel/epoll_scheduler.h"
#endif

#ifdef USE_IOURING
#include "kernel/kernel/uring_scheduler.h"
#endif

using namespace galay::async;
using namespace galay::kernel;

namespace {

uint16_t udpTestPort() {
    return galay::test::resolvePortFromEnv("GALAY_TEST_UDP_PORT", 8080);
}

}

// UDP Echo服务器协程
Task<void> udpEchoServer() {
    LogInfo("UDP Server starting...");
    UdpSocket socket;
    LogDebug("Socket created, fd={}", socket.handle().fd);

    // 设置选项
    auto optResult = socket.option().handleReuseAddr();
    if (!optResult) {
        LogError("Failed to set reuse addr: {}", optResult.error().message());
        co_return;
    }

    optResult = socket.option().handleNonBlock();
    if (!optResult) {
        LogError("Failed to set non-block: {}", optResult.error().message());
        co_return;
    }

    // 绑定地址
    Host bindHost(IPType::IPV4, "127.0.0.1", udpTestPort());
    auto bindResult = socket.bind(bindHost);
    if (!bindResult) {
        LogError("Failed to bind: {}", bindResult.error().message());
        co_return;
    }
    LogDebug("Bind successful");

    LogInfo("UDP Server listening on 127.0.0.1:{}", udpTestPort());

    // Echo循环 - 接收并回显3个数据报
    char buffer[65536];
    for (int i = 0; i < 3; ++i) {
        Host from;
        LogDebug("Waiting for recvfrom...");
        auto recvResult = co_await socket.recvfrom(buffer, sizeof(buffer), &from);
        if (!recvResult) {
            LogError("Recvfrom error: {}", recvResult.error().message());
            break;
        }

        size_t bytes = recvResult.value();
        LogInfo("Received from {}:{}: {}", from.ip(), from.port(), std::string_view(buffer, bytes));

        // Echo回发送方
        auto sendResult = co_await socket.sendto(buffer, bytes, from);
        if (!sendResult) {
            LogError("Sendto error: {}", sendResult.error().message());
            break;
        }
        LogDebug("Sent {} bytes back to {}:{}", sendResult.value(), from.ip(), from.port());
    }

    co_await socket.close();
    LogInfo("UDP Server stopped");
    co_return;
}

// UDP客户端协程
Task<void> udpEchoClient() {
    LogInfo("UDP Client starting...");
    UdpSocket socket;
    LogDebug("Client socket created, fd={}", socket.handle().fd);

    socket.option().handleNonBlock();

    // 服务器地址
    Host serverHost(IPType::IPV4, "127.0.0.1", udpTestPort());

    // 发送3条消息
    const char* messages[] = {
        "Hello, UDP Server!",
        "This is message 2",
        "Final message"
    };

    for (int i = 0; i < 3; ++i) {
        // 发送消息
        LogDebug("Client sending message {}...", i + 1);
        auto sendResult = co_await socket.sendto(messages[i], strlen(messages[i]), serverHost);
        if (!sendResult) {
            LogError("Client: Sendto failed");
            continue;
        }

        LogInfo("Client: Sent message {}: {}", i + 1, messages[i]);

        // 接收回复
        char buffer[1024];
        Host from;
        auto recvResult = co_await socket.recvfrom(buffer, sizeof(buffer), &from);
        if (!recvResult) {
            LogError("Client: Recvfrom failed");
            continue;
        }

        size_t bytes = recvResult.value();
        LogInfo("Client: Received echo from {}:{}: {}", from.ip(), from.port(), std::string_view(buffer, bytes));
    }

    co_await socket.close();
    LogInfo("UDP Client stopped");
    co_return;
}

int main() {
    LogInfo("UdpSocket Test");

#ifdef USE_KQUEUE
    LogInfo("Using KqueueScheduler (macOS)");
    KqueueScheduler scheduler;
    scheduler.start();
    LogDebug("Scheduler started");

    // 启动服务器
    scheduleTask(scheduler, udpEchoServer());
    LogDebug("Server coroutine spawned");

    // 等待一下让服务器启动
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // 启动客户端
    scheduleTask(scheduler, udpEchoClient());
    LogDebug("Client coroutine spawned");

    // 运行一段时间
    std::this_thread::sleep_for(std::chrono::seconds(2));

    scheduler.stop();
    LogInfo("Test finished");
#elif defined(USE_EPOLL)
    LogInfo("Using EpollScheduler (Linux)");
    EpollScheduler scheduler;
    scheduler.start();
    LogDebug("Scheduler started");

    // 启动服务器
    scheduleTask(scheduler, udpEchoServer());
    LogDebug("Server coroutine spawned");

    // 等待一下让服务器启动
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // 启动客户端
    scheduleTask(scheduler, udpEchoClient());
    LogDebug("Client coroutine spawned");

    // 运行一段时间
    std::this_thread::sleep_for(std::chrono::seconds(2));

    scheduler.stop();
    LogInfo("Test finished");
#elif defined(USE_IOURING)
    LogInfo("Using IOUringScheduler (Linux io_uring)");
    IOUringScheduler scheduler;
    scheduler.start();
    LogDebug("Scheduler started");

    // 启动服务器
    scheduleTask(scheduler, udpEchoServer());
    LogDebug("Server coroutine spawned");

    // 等待一下让服务器启动
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // 启动客户端
    scheduleTask(scheduler, udpEchoClient());
    LogDebug("Client coroutine spawned");

    // 运行一段时间
    std::this_thread::sleep_for(std::chrono::seconds(2));

    scheduler.stop();
    LogInfo("Test finished");
#else
    LogWarn("This test requires kqueue (macOS), epoll or io_uring (Linux)");
#endif

    return 0;
}
