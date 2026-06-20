/**
 * @file e1_sendfile.cc
 * @brief 用途：用头文件方式演示 `SendFile` 最小闭环传输。
 * 关键覆盖点：测试文件准备、服务端发送、客户端接收与字节数校验。
 * 通过条件：文件传输完整跑通，接收结果达预期并返回 0。
 */

#include <atomic>
#include <algorithm>
#include <chrono>
#include <fcntl.h>
#include <iostream>
#include <string>
#include <thread>
#include <unistd.h>
#include <sys/stat.h>

#include <galay/cpp/galay-kernel/async/tcp_socket.h>
#include <galay/cpp/galay-kernel/core/task.h>

#ifdef USE_KQUEUE
#include <galay/cpp/galay-kernel/core/kqueue_scheduler.h>
using IOSchedulerType = galay::kernel::KqueueScheduler;
#endif

#ifdef USE_EPOLL
#include <galay/cpp/galay-kernel/core/epoll_scheduler.h>
using IOSchedulerType = galay::kernel::EpollScheduler;
#endif

#ifdef USE_IOURING
#include <galay/cpp/galay-kernel/core/uring_scheduler.h>
using IOSchedulerType = galay::kernel::IOUringScheduler;
#endif

using namespace galay::async;
using namespace galay::kernel;

namespace {
constexpr const char* kTestFile = "/tmp/galay_sendfile_example.dat";
constexpr uint16_t kPort = 9080;
constexpr size_t kFileSize = 1024 * 1024;  // 1 MiB

std::atomic<bool> g_server_ready{false};
std::atomic<bool> g_done{false};
std::atomic<size_t> g_received{0};

bool createTestFile() {
    int fd = ::open(kTestFile, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (fd < 0) {
        return false;
    }

    std::string chunk(4096, 'S');
    size_t written = 0;
    while (written < kFileSize) {
        const size_t want = std::min(chunk.size(), kFileSize - written);
        ssize_t n = ::write(fd, chunk.data(), want);
        if (n <= 0) {
            ::close(fd);
            return false;
        }
        written += static_cast<size_t>(n);
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
        co_await listener.close();
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

    IOSchedulerType scheduler;
    scheduler.start();

    scheduleTask(scheduler, sendfileServer());
    scheduleTask(scheduler, sendfileClient());

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!g_done.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    scheduler.stop();
    ::unlink(kTestFile);

    if (g_received.load(std::memory_order_acquire) != kFileSize) {
        std::cerr << "sendfile example failed\n";
        return 1;
    }

    std::cout << "sendfile example passed\n";
    return 0;
}
