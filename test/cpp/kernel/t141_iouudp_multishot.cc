/**
 * @file t141_iouudp_multishot.cc
 * @brief 验证 io_uring UDP multishot recvmsg 保持数据报边界并正确回填源地址。
 * @details 单个客户端突发发送三个不同长度的数据报；服务端先用小缓冲截断首包，
 *          后续 recvfrom 必须直接交付下一完整数据报，而不能交付首包余量或发生串包。
 */

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/utsname.h>
#include <unistd.h>

#include <galay/cpp/galay-kernel/async/udp_socket.h>
#include <galay/cpp/galay-kernel/core/task.h>
#include "test/cpp/common/stdout_log.h"

#ifdef USE_IOURING
#include <galay/cpp/galay-kernel/core/uring_scheduler.h>
#endif

using namespace galay::async;
using namespace galay::kernel;

namespace {

constexpr std::string_view kFirstPayload = "first-datagram-is-long";
constexpr std::string_view kSecondPayload = "second";
constexpr std::string_view kThirdPayload = "third-datagram";

std::atomic<bool> g_server_ready{false};
std::atomic<bool> g_first_received{false};
std::atomic<bool> g_done{false};
std::atomic<uint16_t> g_server_port{0};
std::atomic<uint16_t> g_first_client_port{0};
std::atomic<uint16_t> g_second_client_port{0};
std::string g_error;

void fail(std::string message)
{
    g_error = std::move(message);
    g_done.store(true, std::memory_order_release);
}

uint16_t socketPort(int fd)
{
    sockaddr_in address{};
    socklen_t length = sizeof(address);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length) != 0) {
        return 0;
    }
    return ntohs(address.sin_port);
}

bool matchesSource(const Host& source, uint16_t expected_port)
{
    const auto* address = reinterpret_cast<const sockaddr_in*>(source.sockAddr());
    return address != nullptr &&
           address->sin_family == AF_INET &&
           ntohs(address->sin_port) == expected_port;
}

#ifdef USE_IOURING
bool hostShouldSupportRecvmsgMultishot()
{
#if IO_URING_VERSION_MAJOR > 2 || \
    (IO_URING_VERSION_MAJOR == 2 && IO_URING_VERSION_MINOR >= 2)
    utsname info{};
    if (::uname(&info) != 0) {
        return false;
    }
    unsigned major = 0;
    unsigned minor = 0;
    if (std::sscanf(info.release, "%u.%u", &major, &minor) != 2) {
        return false;
    }
    return major >= 6;
#else
    return false;
#endif
}
#endif

#ifdef USE_IOURING
Task<void> receiveBurst()
{
    UdpSocket socket;
    auto option = socket.option().handleReuseAddr();
    if (!option) {
        fail("reuse addr failed: " + option.error().message());
        co_return;
    }
    option = socket.option().handleNonBlock();
    if (!option) {
        fail("non-block failed: " + option.error().message());
        co_return;
    }

    Host bind_host(IPType::IPV4, "127.0.0.1", 0);
    auto bind_result = socket.bind(bind_host);
    if (!bind_result) {
        fail("bind failed: " + bind_result.error().message());
        co_return;
    }
    const uint16_t port = socketPort(socket.handle().fd);
    if (port == 0) {
        fail("server getsockname returned port 0");
        co_return;
    }
    g_server_port.store(port, std::memory_order_release);
    g_server_ready.store(true, std::memory_order_release);

    char first[5]{};
    Host first_source;
    auto first_result = co_await socket.recvfrom(first, sizeof(first), &first_source);
    if (!first_result || first_result.value() != sizeof(first) ||
        std::string_view(first, sizeof(first)) != kFirstPayload.substr(0, sizeof(first)) ||
        !matchesSource(first_source, g_first_client_port.load(std::memory_order_acquire))) {
        fail("first truncated datagram or source address mismatch");
        co_return;
    }
    g_first_received.store(true, std::memory_order_release);

    char second[64]{};
    Host second_source;
    auto second_result = co_await socket.recvfrom(second, sizeof(second), &second_source);
    if (!second_result || second_result.value() != kSecondPayload.size() ||
        std::string_view(second, second_result ? second_result.value() : 0) != kSecondPayload ||
        !matchesSource(second_source, g_second_client_port.load(std::memory_order_acquire))) {
        fail("second recvfrom did not preserve the next datagram boundary");
        co_return;
    }

    char third[64]{};
    Host third_source;
    auto third_result = co_await socket.recvfrom(third, sizeof(third), &third_source);
    if (!third_result || third_result.value() != kThirdPayload.size() ||
        std::string_view(third, third_result ? third_result.value() : 0) != kThirdPayload ||
        !matchesSource(third_source, g_first_client_port.load(std::memory_order_acquire))) {
        fail("third recvfrom payload or source address mismatch");
        co_return;
    }

    if (hostShouldSupportRecvmsgMultishot() &&
        !socket.controller()->m_recvfrom_multishot_armed) {
        fail("Linux io_uring test host did not keep UDP multishot recvmsg armed");
        co_return;
    }

    auto close_result = co_await socket.close();
    if (!close_result) {
        fail("server close failed: " + close_result.error().message());
        co_return;
    }
    g_done.store(true, std::memory_order_release);
    co_return;
}
#endif

bool sendDatagram(int fd, const sockaddr_in& target, std::string_view payload)
{
    const ssize_t sent = ::sendto(fd,
                                  payload.data(),
                                  payload.size(),
                                  0,
                                  reinterpret_cast<const sockaddr*>(&target),
                                  sizeof(target));
    return sent == static_cast<ssize_t>(payload.size());
}

bool closeSocket(int fd, const char* label)
{
    if (::close(fd) == 0) {
        return true;
    }
    LogError("failed to close {} socket: errno={}", label, errno);
    return false;
}

}  // namespace

int main()
{
#ifndef USE_IOURING
    LogInfo("T141-IOUringUdpMultishotRuntime skipped: requires io_uring backend");
    return 0;
#else
    IOUringScheduler scheduler;
    auto start_result = scheduler.start();
    if (!start_result) {
        LogError("io_uring scheduler start failed: {}", start_result.error().message());
        return 1;
    }
    if (!scheduleTask(scheduler, receiveBurst())) {
        scheduler.stop();
        LogError("failed to schedule UDP receiver");
        return 1;
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!g_server_ready.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    const uint16_t server_port = g_server_port.load(std::memory_order_acquire);
    if (!g_server_ready.load(std::memory_order_acquire) || server_port == 0) {
        scheduler.stop();
        LogError("UDP receiver did not bind before deadline");
        return 1;
    }

    const int first_client_fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    const int second_client_fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (first_client_fd < 0 || second_client_fd < 0) {
        bool cleanup_ok = true;
        if (first_client_fd >= 0) {
            cleanup_ok = closeSocket(first_client_fd, "first client") && cleanup_ok;
        }
        if (second_client_fd >= 0) {
            cleanup_ok = closeSocket(second_client_fd, "second client") && cleanup_ok;
        }
        scheduler.stop();
        LogError("failed to create UDP client sockets");
        return cleanup_ok ? 1 : 2;
    }
    sockaddr_in first_client_address{};
    first_client_address.sin_family = AF_INET;
    first_client_address.sin_port = 0;
    first_client_address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sockaddr_in second_client_address = first_client_address;
    if (::bind(first_client_fd,
               reinterpret_cast<const sockaddr*>(&first_client_address),
               sizeof(first_client_address)) != 0 ||
        ::bind(second_client_fd,
               reinterpret_cast<const sockaddr*>(&second_client_address),
               sizeof(second_client_address)) != 0) {
        const bool first_closed = closeSocket(first_client_fd, "first client");
        const bool second_closed = closeSocket(second_client_fd, "second client");
        scheduler.stop();
        LogError("failed to bind UDP client sockets");
        return first_closed && second_closed ? 1 : 2;
    }
    g_first_client_port.store(socketPort(first_client_fd), std::memory_order_release);
    g_second_client_port.store(socketPort(second_client_fd), std::memory_order_release);

    sockaddr_in server_address{};
    server_address.sin_family = AF_INET;
    server_address.sin_port = htons(server_port);
    server_address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    bool sent = sendDatagram(first_client_fd, server_address, kFirstPayload);
    while (sent && !g_first_received.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    sent = sent && g_first_received.load(std::memory_order_acquire) &&
           sendDatagram(second_client_fd, server_address, kSecondPayload) &&
           sendDatagram(first_client_fd, server_address, kThirdPayload);
    const bool first_closed = closeSocket(first_client_fd, "first client");
    const bool second_closed = closeSocket(second_client_fd, "second client");
    sent = sent && first_closed && second_closed;
    if (!sent) {
        scheduler.stop();
        LogError("failed to send UDP burst");
        return 1;
    }

    while (!g_done.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    scheduler.stop();

    if (!g_done.load(std::memory_order_acquire)) {
        LogError("UDP multishot test timed out");
        return 1;
    }
    if (!g_error.empty()) {
        LogError("UDP multishot test failed: {}", g_error);
        return 1;
    }
    LogInfo("T141 passed");
    return 0;
#endif
}
