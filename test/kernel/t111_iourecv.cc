/**
 * @file t111_iourecv.cc
 * @brief 用途：验证 io_uring multishot recv + provided buffer ring 能在 controller 内部暂存未消费完的数据。
 * 关键覆盖点：同一 recv CQE 返回大于用户 buffer 的 payload 时，余量会进入 ready recv queue，后续 recv 可直接继续消费。
 * 通过条件：第一次 recv 拿到 payload 前缀、controller ready recv queue 非空、第二次 recv 拿到剩余 payload 且最终 EOF。
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

#include <galay/cpp/galay-kernel/async/tcp_socket.h>
#include <galay/cpp/galay-kernel/core/task.h>
#include "test/stdout_log.h"

#ifdef USE_IOURING
#include <galay/cpp/galay-kernel/core/uring_scheduler.h>
#endif

using namespace galay::async;
using namespace galay::kernel;

namespace {

constexpr size_t kFirstRecvSize = 128;
constexpr size_t kPayloadSize = 4096;

std::atomic<bool> g_listener_ready{false};
std::atomic<bool> g_client_sent{false};
std::atomic<bool> g_done{false};
std::atomic<uint16_t> g_port{0};
std::atomic<uint32_t> g_error_sys{0};

std::mutex g_error_mutex;
std::string g_error_message;

std::string makePayload() {
    std::string payload;
    payload.resize(kPayloadSize);
    for (size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<char>('a' + (i % 26));
    }
    return payload;
}

const std::string& payload() {
    static const std::string kPayload = makePayload();
    return kPayload;
}

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

bool sendAll(int fd, const char* data, size_t length) {
    size_t sent = 0;
    while (sent < length) {
        const ssize_t n = ::send(fd, data + sent, length - sent, 0);
        if (n <= 0) {
            return false;
        }
        sent += static_cast<size_t>(n);
    }
    return true;
}

#ifdef USE_IOURING
Task<void> recvTwiceWithStaging() {
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
        const uint32_t sys = static_cast<uint32_t>(acceptResult.error().code() >> 32);
        recordFailure("accept failed: " + acceptResult.error().message(), sys);
        co_await listener.close();
        co_return;
    }

    TcpSocket client(acceptResult.value());
    client.option().handleNonBlock();

    const auto sent_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    while (!g_client_sent.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < sent_deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (!g_client_sent.load(std::memory_order_acquire)) {
        recordFailure("client did not finish sending before recv");
        co_await client.close();
        co_await listener.close();
        co_return;
    }

    char first[kFirstRecvSize];
    auto firstRecv = co_await client.recv(first, sizeof(first));
    if (!firstRecv) {
        const uint32_t sys = static_cast<uint32_t>(firstRecv.error().code() >> 32);
        recordFailure("first recv failed: " + firstRecv.error().message(), sys);
        co_await client.close();
        co_await listener.close();
        co_return;
    }
    if (firstRecv.value() != sizeof(first)) {
        recordFailure("first recv returned unexpected length: " + std::to_string(firstRecv.value()));
        co_await client.close();
        co_await listener.close();
        co_return;
    }
    if (std::memcmp(first, payload().data(), sizeof(first)) != 0) {
        recordFailure("first recv payload mismatch");
        co_await client.close();
        co_await listener.close();
        co_return;
    }
    if (client.controller()->m_ready_recvs.empty()) {
        recordFailure("expected staged ready recv data after partial consume");
        co_await client.close();
        co_await listener.close();
        co_return;
    }

    std::vector<char> second(payload().size() - sizeof(first));
    auto secondRecv = co_await client.recv(second.data(), second.size());
    if (!secondRecv) {
        const uint32_t sys = static_cast<uint32_t>(secondRecv.error().code() >> 32);
        recordFailure("second recv failed: " + secondRecv.error().message(), sys);
        co_await client.close();
        co_await listener.close();
        co_return;
    }
    if (secondRecv.value() != second.size()) {
        recordFailure("second recv returned unexpected length: " + std::to_string(secondRecv.value()));
        co_await client.close();
        co_await listener.close();
        co_return;
    }
    if (std::memcmp(second.data(),
                    payload().data() + sizeof(first),
                    second.size()) != 0) {
        recordFailure("second recv payload mismatch");
        co_await client.close();
        co_await listener.close();
        co_return;
    }
    char eof = '\0';
    auto eofRecv = co_await client.recv(&eof, 1);
    if (!eofRecv) {
        const uint32_t sys = static_cast<uint32_t>(eofRecv.error().code() >> 32);
        recordFailure("eof recv failed: " + eofRecv.error().message(), sys);
        co_await client.close();
        co_await listener.close();
        co_return;
    }
    if (eofRecv.value() != 0) {
        recordFailure("expected EOF after staged payload drained");
        co_await client.close();
        co_await listener.close();
        co_return;
    }
    if (!client.controller()->m_ready_recvs.empty()) {
        recordFailure("ready recv queue should be empty after EOF is consumed");
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
    LogInfo("T111-IOUringMultishotRecvRuntime skipped: requires io_uring backend");
    return 0;
#else
    LogInfo("=== T111: io_uring multishot recv runtime regression ===");

    IOUringScheduler scheduler;
    scheduler.start();
    scheduleTask(scheduler, recvTwiceWithStaging());

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
        const int err = errno;
        ::close(fd);
        scheduler.stop();
        LogError("client connect failed: {}", std::strerror(err));
        return 1;
    }

    if (!sendAll(fd, payload().data(), payload().size())) {
        const int err = errno;
        ::close(fd);
        scheduler.stop();
        LogError("client send failed: {}", std::strerror(err));
        return 1;
    }

    g_client_sent.store(true, std::memory_order_release);
    ::shutdown(fd, SHUT_WR);
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

    if (!g_done.load(std::memory_order_acquire)) {
        LogError("test timed out waiting for recv completion");
        return 1;
    }

    if (!error_message.empty()) {
        LogError("unexpected failure: {}", error_message);
        return 1;
    }

    LogInfo("T111 passed");
    return 0;
#endif
}
