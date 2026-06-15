/**
 * @file t114_iouzc.cc
 * @brief 用途：验证 io_uring send_zc 在连续大包发送场景下不会被 notification CQE 干扰完成语义。
 * 关键覆盖点：同一 socket 上连续两次大包 send、notification CQE 晚到时不重复完成/不误伤下一次 send。
 * 通过条件：客户端按顺序收到两段完整 payload，服务端协程正常结束，测试返回 0。
 */

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "galay-kernel/async/tcp_socket.h"
#include "galay-kernel/core/task.h"
#include "test/stdout_log.h"

#ifdef USE_IOURING
#include "galay-kernel/core/uring_scheduler.h"
#endif

using namespace galay::async;
using namespace galay::kernel;

namespace {

constexpr size_t kChunkSize = 64 * 1024;

std::atomic<bool> g_listener_ready{false};
std::atomic<bool> g_done{false};
std::atomic<uint16_t> g_port{0};

std::mutex g_error_mutex;
std::string g_error_message;

std::string makePayload(char fill) {
    return std::string(kChunkSize, fill);
}

const std::string& firstPayload() {
    static const std::string kPayload = makePayload('a');
    return kPayload;
}

const std::string& secondPayload() {
    static const std::string kPayload = makePayload('z');
    return kPayload;
}

void recordFailure(const std::string& message) {
    {
        std::lock_guard<std::mutex> lock(g_error_mutex);
        g_error_message = message;
    }
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

#ifdef USE_IOURING
Task<void> sendTwoChunks() {
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

    Host clientHost;
    auto acceptResult = co_await listener.accept(&clientHost);
    if (!acceptResult) {
        recordFailure("accept failed: " + acceptResult.error().message());
        co_await listener.close();
        co_return;
    }

    TcpSocket client(acceptResult.value());
    client.option().handleNonBlock();

    auto first = co_await client.send(firstPayload().data(), firstPayload().size());
    if (!first || first.value() != firstPayload().size()) {
        recordFailure("first send failed or partial");
        co_await client.close();
        co_await listener.close();
        co_return;
    }

    auto second = co_await client.send(secondPayload().data(), secondPayload().size());
    if (!second || second.value() != secondPayload().size()) {
        recordFailure("second send failed or partial");
        co_await client.close();
        co_await listener.close();
        co_return;
    }

    co_await client.close();
    co_await listener.close();
    g_done.store(true, std::memory_order_release);
    co_return;
}
#endif

}  // namespace

int main() {
#ifndef USE_IOURING
    LogInfo("T114-IOUringSendZcRuntime skipped: requires io_uring backend");
    return 0;
#else
    LogInfo("=== T114: io_uring send_zc runtime regression ===");

    IOUringScheduler scheduler;
    scheduler.start();
    scheduleTask(scheduler, sendTwoChunks());

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

    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        scheduler.stop();
        LogError("failed to create client socket");
        return 1;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(fd);
        scheduler.stop();
        LogError("client connect failed");
        return 1;
    }

    std::string received;
    received.resize(firstPayload().size() + secondPayload().size());
    size_t offset = 0;
    while (offset < received.size()) {
        const ssize_t n = ::recv(fd, received.data() + offset, received.size() - offset, 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (n == 0) {
            break;
        }
        offset += static_cast<size_t>(n);
    }
    ::close(fd);

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

    if (!error_message.empty()) {
        LogError("unexpected failure: {}", error_message);
        return 1;
    }
    if (!g_done.load(std::memory_order_acquire)) {
        LogError("server task did not finish");
        return 1;
    }
    if (offset != received.size()) {
        LogError("received {} bytes, expected {}", offset, received.size());
        return 1;
    }
    if (std::memcmp(received.data(), firstPayload().data(), firstPayload().size()) != 0) {
        LogError("first payload mismatch");
        return 1;
    }
    if (std::memcmp(received.data() + firstPayload().size(),
                    secondPayload().data(),
                    secondPayload().size()) != 0) {
        LogError("second payload mismatch");
        return 1;
    }

    LogInfo("T114 passed");
    return 0;
#endif
}
