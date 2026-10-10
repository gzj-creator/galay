/**
 * @file include/e2_echo.cc
 * @brief SSL客户端示例
 * @details 演示如何使用SslSocket创建一个SSL客户端连接服务器
 *
 * 使用场景：
 *   - 学习SSL客户端基本用法
 *   - 理解SSL握手流程
 *   - 作为HTTPS客户端的基础模板
 *
 * 运行方式：
 *   ./e2_echo_include <host> <port> [ca_cert]
 *   例如: ./e2_echo_include localhost 8443 certs/ca.crt
 */

#include <iostream>
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
#include <galay/cpp/galay-kernel/core/uring_scheduler.h>
using IOSchedulerType = galay::kernel::IOUringScheduler;
#endif

using namespace galay::ssl;
using namespace galay::kernel;

/**
 * @brief SSL客户端协程
 * @param ctx SSL 上下文
 * @param host 目标主机地址
 * @param port 端口号
 * @return 执行该操作的协程任务，完成后无结果值
 */
Task<void> ssl_client(SslContext* ctx, const std::string& host, uint16_t port) {
    SslSocket socket(ctx);

    if (!socket.is_valid()) {
        co_return;
    }

    socket.option().handle_non_block();
    socket.set_hostname(host);  // 设置SNI

    std::cout << "Connecting to " << host << ":" << port << "..." << std::endl;

    // 连接服务器
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

    // 发送测试消息
    std::string message = "Hello, SSL Server!";
    std::cout << "Sending: " << message << std::endl;

    auto sendResult = co_await socket.send(message.c_str(), message.size());
    if (!sendResult) {
        co_await socket.close();
        co_return;
    }

    std::cout << "Sent " << sendResult.value() << " bytes" << std::endl;

    // 接收响应
    char buffer[4096];
    auto recvResult = co_await socket.recv(buffer, sizeof(buffer));
    if (!recvResult) {
    } else {
        auto& bytes = recvResult.value();
        std::cout << "Received: " << bytes.to_string_view() << std::endl;
    }

    // 关闭连接
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

    // 创建SSL上下文
    SslContext ctx(SslMethod::TLS_Client);
    if (!ctx.is_valid()) {
        return 1;
    }

    // 加载CA证书（可选）
    if (!caCert.empty()) {
        auto caResult = ctx.load_ca_certificate(caCert);
        if (!caResult) {
            return 1;
        }
        ctx.set_verify_mode(SslVerifyMode::Peer);
    } else {
        // 不验证服务器证书（仅用于测试）
        ctx.set_verify_mode(SslVerifyMode::None);
    }

    // 创建调度器
    IOSchedulerType scheduler;
    scheduler.start();

    // 启动客户端
    schedule_task(scheduler, ssl_client(&ctx, host, port));

    // 等待完成
    std::this_thread::sleep_for(std::chrono::seconds(2));

    scheduler.stop();
    return 0;
}
