/**
 * @file e1_sendfile.cc
 * @brief 用途：用模块导入方式演示 `SendFile` 最小闭环传输。
 * 关键覆盖点：模块门面 `galay.kernel`、文件准备、服务端发送与客户端接收。
 * 通过条件：文件传输完整跑通，接收结果达预期并返回 0。
 */

import galay.kernel;

#include <coroutine>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <string>
#include <thread>
#include <unistd.h>
#include <sys/stat.h>

using namespace galay::kernel;
using namespace galay::async;

namespace {
constexpr const char* kTestFile = "/tmp/galay_module_sendfile.dat";
constexpr uint16_t kPort = 9081;
constexpr size_t kFileSize = 1024 * 1024;  // 1 MiB

std::atomic<bool> g_server_ready{false};
std::atomic<bool> g_done{false};
std::atomic<size_t> g_received{0};

bool createTestFile() {
    int fd = ::open(kTestFile, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        return false;
    }

    char block[4096];
    std::memset(block, 'S', sizeof(block));
    size_t written = 0;
    while (written < kFileSize) {
        size_t n = std::min(sizeof(block), kFileSize - written);
        if (::write(fd, block, n) <= 0) {
            ::close(fd);
            return false;
        }
        written += n;
    }

    ::close(fd);
    return true;
}

Task<void> sendfileServer() {
    TcpSocket listener;
    listener.option().handleReuseAddr();
    listener.option().handleNonBlock();

    auto bindResult = listener.bind(Host(IPType::IPV4, "127.0.0.1", kPort));
    if (!bindResult) {
        g_done.store(true, std::memory_order_release);
        co_return;
    }

    auto listenResult = listener.listen(16);
    if (!listenResult) {
        g_done.store(true, std::memory_order_release);
        co_return;
    }

    g_server_ready.store(true, std::memory_order_release);

    Host peer;
    auto accepted = co_await listener.accept(&peer);
    if (!accepted) {
        g_done.store(true, std::memory_order_release);
        co_return;
    }

    TcpSocket client(accepted.value());
    client.option().handleNonBlock();

    int fd = ::open(kTestFile, O_RDONLY);
    if (fd < 0) {
        co_await client.close();
        co_await listener.close();
        g_done.store(true, std::memory_order_release);
        co_return;
    }

    size_t sent_total = 0;
    while (sent_total < kFileSize) {
        auto sent = co_await client.sendfile(fd, static_cast<off_t>(sent_total), kFileSize - sent_total);
        if (!sent || sent.value() == 0) {
            break;
        }
        sent_total += sent.value();
    }

    ::close(fd);
    co_await client.close();
    co_await listener.close();

    g_done.store(true, std::memory_order_release);
}

Task<void> sendfileClient() {
    while (!g_server_ready.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    TcpSocket socket;
    socket.option().handleNonBlock();

    auto connected = co_await socket.connect(Host(IPType::IPV4, "127.0.0.1", kPort));
    if (!connected) {
        g_done.store(true, std::memory_order_release);
        co_return;
    }

    char buffer[8192];
    size_t total = 0;
    while (total < kFileSize) {
        auto recv = co_await socket.recv(buffer, sizeof(buffer));
        if (!recv || recv.value() == 0) {
            break;
        }
        total += recv.value();
    }

    g_received.store(total, std::memory_order_release);
    co_await socket.close();
}
}  // namespace

int main() {
    if (!createTestFile()) {
        std::cerr << "failed to create test file\n";
        return 1;
    }

    Runtime runtime = RuntimeBuilder().ioSchedulerCount(1).computeSchedulerCount(1).build();
    runtime.start();

    auto* io = runtime.getNextIOScheduler();
    scheduleTask(io, sendfileServer());
    scheduleTask(io, sendfileClient());

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!g_done.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    runtime.stop();
    ::unlink(kTestFile);

    std::cout << "sendfile import example received bytes: " << g_received.load() << "\n";
    return 0;
}
