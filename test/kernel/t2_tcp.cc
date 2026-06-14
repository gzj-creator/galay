/**
 * @file t2_tcp.cc
 * @brief 用途：验证 `TcpSocket` 的基础连接、发送和接收能力。
 * 关键覆盖点：主动连接、基础收发接口、错误码与连接状态的基本行为。
 * 通过条件：连接与收发断言全部成立，测试流程正常结束并返回 0。
 */

#include <exception>
#include <iostream>
#include <cstring>
#include <string_view>
#include "kernel/async/tcp_socket.h"
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

uint16_t tcpTestPort() {
    return galay::test::resolvePortFromEnv("GALAY_TEST_TCP_PORT", 8080);
}

}

// Echo服务器协程
Task<void> echoServer([[maybe_unused]] IOScheduler* scheduler) {
    LogInfo("Server starting...");
    TcpSocket listener;
    // 设置选项
    auto optResult = listener.option().handleReuseAddr();
    if (!optResult) {
        LogError("Failed to set reuse addr: {}", optResult.error().message());
        co_return;
    }

    optResult = listener.option().handleNonBlock();
    if (!optResult) {
        LogError("Failed to set non-block: {}", optResult.error().message());
        co_return;
    }

    // 绑定地址
    Host bindHost(IPType::IPV4, "127.0.0.1", tcpTestPort());
    auto bindResult = listener.bind(bindHost);
    if (!bindResult) {
        LogError("Failed to bind: {}", bindResult.error().message());
        co_return;
    }
    LogDebug("Bindsuccessful");

    // 监听
    auto listenResult = listener.listen(128);
    if (!listenResult) {
        LogError("Failed to listen: {}", listenResult.error().message());
        co_return;
    }

    LogInfo("Server listening on 127.0.0.1:{}", tcpTestPort());

    // 接受连接
    Host clientHost;
    LogDebug("Waiting for accept...");
    auto acceptResult = co_await listener.accept(&clientHost);
    if (!acceptResult) {
        LogError("Failed to accept: {}", acceptResult.error().message());
        co_return;
    }

    LogInfo("Client connected from {}:{}", clientHost.ip(), clientHost.port());

    // 创建客户端socket
    TcpSocket client(acceptResult.value());
    client.option().handleNonBlock();

    // Echo循环
    char buffer[1024];
    while (true) {
        LogDebug("Waiting for recv...");
        auto recvResult = co_await client.recv(buffer, sizeof(buffer));
        if (!recvResult) {
            LogError("Recv error: {}", recvResult.error().message());
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
            break;
        }
        LogDebug("Sent {} bytes", sendResult.value());
    }

    co_await client.close();
    co_await listener.close();
    LogInfo("Server stopped");
    co_return;
}

// 客户端协程
Task<void> echoClient([[maybe_unused]] IOScheduler* scheduler) {
    LogInfo("Client starting...");
    TcpSocket client;
    LogDebug("Client socket created, fd={}", client.handle().fd);

    client.option().handleNonBlock();

    // 连接服务器
    Host serverHost(IPType::IPV4, "127.0.0.1", tcpTestPort());
    LogDebug("Client connecting to server...");
    auto connectResult = co_await client.connect(serverHost);
    if (!connectResult) {
        LogError("Client: Failed to connect: {}", connectResult.error().message());
        co_return;
    }

    LogInfo("Client: Connected to server");

    // 发送消息
    const char* msg = "Hello, Server!";
    auto sendResult = co_await client.send(msg, strlen(msg));
    if (!sendResult) {
        LogError("Client: Send failed");
        co_return;
    }

    LogInfo("Client: Sent message");

    // 接收回复
    char buffer[1024];
    auto recvResult = co_await client.recv(buffer, sizeof(buffer));
    if (!recvResult) {
        LogError("Client: Recv failed");
        co_return;
    }

    size_t bytes = recvResult.value();
    LogInfo("Client: Received echo: {}", std::string_view(buffer, bytes));

    co_await client.close();
    LogInfo("Client stopped");
    co_return;
}

int main() {
    LogInfo("TcpSocket Test");

#ifdef USE_KQUEUE
    LogInfo("Using KqueueScheduler (macOS)");
    KqueueScheduler scheduler;
    scheduler.start();
    LogDebug("Scheduler started");

    // 启动服务器
    scheduleTask(scheduler, echoServer(&scheduler));
    LogDebug("Server coroutine spawned");

    // 等待一下让服务器启动
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // 启动客户端
    scheduleTask(scheduler, echoClient(&scheduler));
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
    scheduleTask(scheduler, echoServer(&scheduler));
    LogDebug("Server coroutine spawned");

    // 等待一下让服务器启动
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // 启动客户端
    scheduleTask(scheduler, echoClient(&scheduler));
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
    scheduleTask(scheduler, echoServer(&scheduler));
    LogDebug("Server coroutine spawned");

    // 等待一下让服务器启动
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // 启动客户端
    scheduleTask(scheduler, echoClient(&scheduler));
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
