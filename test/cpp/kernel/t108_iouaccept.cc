/**
 * @file t108_iouaccept.cc
 * @brief 用途：验证 io_uring multishot accept 在同一 listener 上能持续交付多个连接。
 * 关键覆盖点：同一个 multishot accept SQE 连续产出多个 accept 结果、第二次 accept 不丢失。
 * 通过条件：两个独立客户端连接都能被服务端 accept 到，测试返回 0。
 */

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <galay/cpp/galay-kernel/async/tcp_socket.h>
#include <galay/cpp/galay-kernel/core/task.h>
#include "test/cpp/common/stdout_log.h"

#ifdef USE_IOURING
#include <galay/cpp/galay-kernel/core/uring_scheduler.h>
#endif

using namespace galay::async;
using namespace galay::kernel;

namespace {

std::atomic<bool> g_listener_ready{false};
std::atomic<bool> g_waiting_second_accept{false};
std::atomic<bool> g_done{false};
std::atomic<int> g_accepted_count{0};
std::atomic<uint16_t> g_port{0};
std::atomic<uint32_t> g_error_sys{0};

std::mutex g_error_mutex;
std::string g_error_message;

void recordFailure(const std::string& message, uint32_t sys = 0) {
    {
        std::lock_guard<std::mutex> lock(g_error_mutex);
        g_error_message = message;
    }
    g_error_sys.store(sys, std::memory_order_release);
    g_done.store(true, std::memory_order_release);
}

uint16_t boundPort(int fd) {
    sockaddr_in addr{};
    socklen_t len = sizeof(addr);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
        return 0;
    }
    return ntohs(addr.sin_port);
}

bool connectOnce(uint16_t port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return false;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    const bool ok = (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
    ::close(fd);
    return ok;
}

#ifdef USE_IOURING
Task<void> acceptTwice() {
    TcpSocket listener;

    auto opt = listener.option().handleReuseAddr();
    if (!opt) {
        recordFailure("reuse addr failed: " + opt.error().message());
        co_return;
    }

    opt = listener.option().handleNonBlock();
    if (!opt) {
        recordFailure("non-block failed: " + opt.error().message());
        co_return;
    }

    Host bindHost(IPType::IPV4, "127.0.0.1", 0);
    auto bindResult = listener.bind(bindHost);
    if (!bindResult) {
        recordFailure("bind failed: " + bindResult.error().message());
        co_return;
    }

    auto listenResult = listener.listen(16);
    if (!listenResult) {
        recordFailure("listen failed: " + listenResult.error().message());
        co_return;
    }

    const uint16_t port = boundPort(listener.handle().fd);
    if (port == 0) {
        recordFailure("getsockname returned port 0");
        co_await listener.close();
        co_return;
    }

    g_port.store(port, std::memory_order_release);
    g_listener_ready.store(true, std::memory_order_release);

    Host firstClientHost;
    auto firstAccept = co_await listener.accept(&firstClientHost);
    if (!firstAccept) {
        const uint32_t sys = static_cast<uint32_t>(firstAccept.error().code() >> 32);
        recordFailure("first accept failed: " + firstAccept.error().message(), sys);
        co_await listener.close();
        co_return;
    }

    ++g_accepted_count;
    ::close(firstAccept.value().fd);
    g_waiting_second_accept.store(true, std::memory_order_release);

    Host secondClientHost;
    auto secondAccept = co_await listener.accept(&secondClientHost);
    if (!secondAccept) {
        const uint32_t sys = static_cast<uint32_t>(secondAccept.error().code() >> 32);
        recordFailure("second accept failed: " + secondAccept.error().message(), sys);
        co_await listener.close();
        co_return;
    }

    ++g_accepted_count;
    ::close(secondAccept.value().fd);

    co_await listener.close();
    g_done.store(true, std::memory_order_release);
    co_return;
}
#endif

}  // namespace

int main() {
#ifndef USE_IOURING
    LogInfo("T108-IOUringMultishotAcceptRuntime skipped: requires io_uring backend");
    return 0;
#else
    LogInfo("=== T108: io_uring multishot accept runtime regression ===");

    IOUringScheduler scheduler;
    scheduler.start();
    scheduleTask(scheduler, acceptTwice());

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!g_listener_ready.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    const uint16_t port = g_port.load(std::memory_order_acquire);
    if (!g_listener_ready.load(std::memory_order_acquire) || port == 0) {
        scheduler.stop();
        LogError("listener did not become ready");
        return 1;
    }

    if (!connectOnce(port)) {
        scheduler.stop();
        LogError("first client connect failed");
        return 1;
    }

    while (!g_waiting_second_accept.load(std::memory_order_acquire) &&
           !g_done.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    if (!g_done.load(std::memory_order_acquire) && !connectOnce(port)) {
        scheduler.stop();
        LogError("second client connect failed");
        return 1;
    }

    while (!g_done.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    scheduler.stop();

    std::string error_message;
    {
        std::lock_guard<std::mutex> lock(g_error_mutex);
        error_message = g_error_message;
    }

    if (!g_done.load(std::memory_order_acquire)) {
        LogError("test timed out waiting for second accept");
        return 1;
    }

    if (g_accepted_count.load(std::memory_order_acquire) != 2) {
        LogError("accepted {} connections, expected 2", g_accepted_count.load());
        if (!error_message.empty()) {
            LogError("failure: {}", error_message);
        }
        return 1;
    }

    if (!error_message.empty()) {
        LogError("unexpected failure: {}", error_message);
        return 1;
    }

    LogInfo("T108 passed");
    return 0;
#endif
}
