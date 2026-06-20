/**
 * @file include/e1_echo.cc
 * @brief SSL Echo服务器示例
 * @details 演示如何使用SslSocket创建一个简单的SSL Echo服务器
 *
 * 使用场景：
 *   - 学习SSL服务器基本用法
 *   - 理解SSL握手和异步IO的配合
 *   - 作为HTTPS等SSL服务器的基础模板
 *
 * 运行方式：
 *   ./e1_echo_include <port> <cert_file> <key_file>
 *   例如: ./e1_echo_include 8443 certs/server.crt certs/server.key
 */

#include <iostream>
#include <csignal>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <galay/cpp/galay-ssl/async/ssl_socket.h>
#include <galay/cpp/galay-ssl/ssl/ssl_context.h>
#include <galay/cpp/galay-kernel/core/task.h>

#ifdef USE_KQUEUE
#include <galay/cpp/galay-kernel/core/kqueue_scheduler.h>
using IOSchedulerType = galay::kernel::KqueueScheduler;
#elif defined(USE_EPOLL)
#include <galay/cpp/galay-kernel/core/epoll_scheduler.h>
using IOSchedulerType = galay::kernel::EpollScheduler;
#elif defined(USE_IOURING)
#include <galay/cpp/galay-kernel/core/io_uring_scheduler.h>
using IOSchedulerType = galay::kernel::IOUringScheduler;
#endif

using namespace galay::ssl;
using namespace galay::kernel;

std::atomic<bool> g_running{true};

void logErrno(const char* prefix) {
    std::cerr << prefix << ": errno=" << errno << " (" << std::strerror(errno) << ")" << std::endl;
}

void signalHandler(int) {
    g_running = false;
}

/**
 * @brief 处理单个客户端连接
 * @param ctx SSL上下文
 * @param handle 客户端socket句柄
 */
Task<void> handleClient(SslContext* ctx, GHandle handle) {
    SslSocket client(ctx, handle);
    client.option().handleNonBlock();

    auto result = co_await client.handshake();
    if (!result) {
        co_await client.close();
        co_return;
    }

    std::cout << "Client connected, SSL handshake completed" << std::endl;

    // Echo循环
    char buffer[4096];
    while (g_running) {
        auto recvResult = co_await client.recv(buffer, sizeof(buffer));
        if (!recvResult) {
            break;
        }

        auto& bytes = recvResult.value();
        if (bytes.size() == 0) {
            std::cout << "Client disconnected" << std::endl;
            break;
        }

        std::cout << "Received: " << bytes.toStringView() << std::endl;

        // 回显数据
        auto sendResult = co_await client.send(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        if (!sendResult) {
            break;
        }
    }

    co_await client.shutdown();
    co_await client.close();
}

/**
 * @brief SSL Echo服务器协程
 */
Task<void> sslEchoServer(IOSchedulerType* scheduler, SslContext* ctx, uint16_t port) {
    SslSocket listener(ctx);

    if (!listener.isValid()) {
        co_return;
    }

    listener.option().handleReuseAddr();
    listener.option().handleNonBlock();

    auto bindResult = listener.bind(Host(IPType::IPV4, "0.0.0.0", port));
    if (!bindResult) {
        logErrno("bind failed");
        co_return;
    }

    auto listenResult = listener.listen(128);
    if (!listenResult) {
        logErrno("listen failed");
        co_return;
    }

    std::cout << "SSL Echo Server listening on port " << port << std::endl;

    while (g_running) {
        Host clientHost;
        auto acceptResult = co_await listener.accept(&clientHost);
        if (!acceptResult) {
            logErrno("accept failed");
            continue;
        }
        std::cout << "New connection from " << clientHost.ip()
                  << ":" << clientHost.port() << std::endl;
        if (!scheduleTask(scheduler, handleClient(ctx, acceptResult.value()))) {
            std::cerr << "spawn failed for client handler" << std::endl;
        }
    }

    co_await listener.close();
    std::cout << "Server stopped" << std::endl;
}

int main(int argc, char* argv[]) {
    if (argc < 4) {
        return 1;
    }

    uint16_t port = static_cast<uint16_t>(std::stoi(argv[1]));
    std::string certFile = argv[2];
    std::string keyFile = argv[3];

    // 设置信号处理
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);
    signal(SIGPIPE, SIG_IGN);

    // 创建SSL上下文
    SslContext ctx(SslMethod::TLS_Server);
    if (!ctx.isValid()) {
        return 1;
    }

    auto certResult = ctx.loadCertificate(certFile);
    if (!certResult) {
        return 1;
    }

    auto keyResult = ctx.loadPrivateKey(keyFile);
    if (!keyResult) {
        return 1;
    }

    // 创建调度器
    IOSchedulerType scheduler;
    scheduler.start();

    // 启动服务器
    scheduleTask(scheduler, sslEchoServer(&scheduler, &ctx, port));

    // 等待退出
    std::cout << "Press Ctrl+C to stop server..." << std::endl;
    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    scheduler.stop();
    return 0;
}
