/**
 * @file import/e2_echo.cc
 * @brief SSL 客户端示例（C++23 import 版本）
 *
 * 运行方式：
 *   ./e2_echo_import <host> <port> [ca_cert]
 *   例如: ./e2_echo_import localhost 8443 certs/ca.crt
 */

#include <iostream>
#include <string>
#include <thread>
#include <chrono>

import galay.ssl;

#include <kernel/kernel/task.h>

#ifdef USE_KQUEUE
#include <kernel/kernel/kqueue_scheduler.h>
using IOSchedulerType = galay::kernel::KqueueScheduler;
#elif defined(USE_EPOLL)
#include <kernel/kernel/epoll_scheduler.h>
using IOSchedulerType = galay::kernel::EpollScheduler;
#elif defined(USE_IOURING)
#include <kernel/kernel/io_uring_scheduler.h>
using IOSchedulerType = galay::kernel::IOUringScheduler;
#endif

using namespace galay::ssl;
using namespace galay::kernel;

Task<void> sslClient(SslContext* ctx, const std::string& host, uint16_t port) {
    SslSocket socket(ctx);

    if (!socket.isValid()) {
        co_return;
    }

    socket.option().handleNonBlock();
    socket.setHostname(host);

    std::cout << "Connecting to " << host << ":" << port << "..." << std::endl;

    auto connectResult = co_await socket.connect(Host(IPType::IPV4, host, port));
    if (!connectResult) {
        co_await socket.close();
        co_return;
    }

    std::cout << "TCP connected, starting SSL handshake..." << std::endl;

    auto result = co_await socket.handshake();
    if (!result) {
        co_await socket.close();
        co_return;
    }

    std::cout << "SSL handshake completed!" << std::endl;

    std::string message = "Hello, SSL Server!";
    std::cout << "Sending: " << message << std::endl;

    auto sendResult = co_await socket.send(message.c_str(), message.size());
    if (!sendResult) {
        co_await socket.close();
        co_return;
    }

    std::cout << "Sent " << sendResult.value() << " bytes" << std::endl;

    char buffer[4096];
    auto recvResult = co_await socket.recv(buffer, sizeof(buffer));
    if (recvResult) {
        auto& bytes = recvResult.value();
        std::cout << "Received: " << bytes.toStringView() << std::endl;
    }

    co_await socket.shutdown();
    co_await socket.close();
    std::cout << "Connection closed" << std::endl;
}

int main(int argc, char* argv[]) {
    if (argc < 3) {
        return 1;
    }

    std::string host = argv[1];
    uint16_t port = static_cast<uint16_t>(std::stoi(argv[2]));
    std::string caCert = argc > 3 ? argv[3] : "";

    SslContext ctx(SslMethod::TLS_Client);
    if (!ctx.isValid()) {
        return 1;
    }

    if (!caCert.empty()) {
        auto caResult = ctx.loadCACertificate(caCert);
        if (!caResult) {
            return 1;
        }
        ctx.setVerifyMode(SslVerifyMode::Peer);
    } else {
        ctx.setVerifyMode(SslVerifyMode::None);
    }

    IOSchedulerType scheduler;
    scheduler.start();

    scheduleTask(scheduler, sslClient(&ctx, host, port));

    std::this_thread::sleep_for(std::chrono::seconds(2));

    scheduler.stop();
    return 0;
}
