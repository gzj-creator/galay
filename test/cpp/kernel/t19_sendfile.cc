/**
 * @file t19_sendfile.cc
 * @brief 用途：验证 `sendfile` 基础路径能够完成文件到网络的零拷贝传输。
 * 关键覆盖点：文件准备、发送端直传、接收端完整读回以及字节数校验。
 * 通过条件：接收总字节数与源文件一致，测试正常结束并返回 0。
 */

#include <iostream>
#include <fstream>
#include <atomic>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <thread>
#include <chrono>
#include <galay/cpp/galay-kernel/async/tcp_socket.h>
#include <galay/cpp/galay-kernel/core/task.h>
#include "test/cpp/common/stdout_log.h"
#include "result_writer.h"

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

const char* TEST_FILE = "/tmp/galay_sendfile_test.dat";
const char* RECEIVED_FILE = "/tmp/galay_sendfile_received.dat";
const uint16_t TEST_PORT = 9090;

std::atomic<int> g_passed{0};
std::atomic<int> g_failed{0};
std::atomic<int> g_total{0};
std::atomic<bool> g_server_ready{false};
std::atomic<bool> g_test_done{false};

// 创建测试文件
void createTestFile(size_t size) {
    std::ofstream ofs(TEST_FILE, std::ios::binary);
    for (size_t i = 0; i < size; ++i) {
        ofs.put(static_cast<char>(i % 256));
    }
    ofs.close();
    LogInfo("Created test file: {} ({} bytes)", TEST_FILE, size);
}

// 验证文件
bool verifyFile(size_t expected_size) {
    std::ifstream ifs(RECEIVED_FILE, std::ios::binary);
    if (!ifs) return false;

    size_t actual_size = 0;
    char ch;
    while (ifs.get(ch)) {
        if (static_cast<unsigned char>(ch) != (actual_size % 256)) {
            LogError("Data mismatch at byte {}", actual_size);
            return false;
        }
        actual_size++;
    }

    return actual_size == expected_size;
}

// 服务器处理客户端
Task<void> handleClient(TcpSocket client, size_t file_size) {
    int file_fd = open(TEST_FILE, O_RDONLY);
    if (file_fd < 0) {
        LogError("Failed to open file");
        co_await client.close();
        co_return;
    }

    // 发送文件大小
    uint64_t size = file_size;
    co_await client.send(reinterpret_cast<const char*>(&size), sizeof(size));

    // 使用 sendfile 发送
    size_t total_sent = 0;
    off_t offset = 0;

    while (total_sent < file_size) {
        size_t chunk = std::min(file_size - total_sent, size_t(1024 * 1024));
        auto result = co_await client.sendfile(file_fd, offset, chunk);

        if (!result) {
            LogError("Sendfile failed: {}", result.error().message());
            break;
        }

        size_t sent = result.value();
        if (sent == 0) break;

        total_sent += sent;
        offset += sent;
    }

    close(file_fd);
    co_await client.close();

    if (total_sent == file_size) {
        LogInfo("✓ Server sent {} bytes successfully", total_sent);
    } else {
        LogError("✗ Server sent incomplete: {}/{}", total_sent, file_size);
        g_failed++;
    }
}

// 服务器
Task<void> server(size_t file_size) {
    TcpSocket listener;
    listener.option().handleReuseAddr();
    listener.option().handleNonBlock();

    Host bindHost(IPType::IPV4, "127.0.0.1", TEST_PORT);
    if (!listener.bind(bindHost)) {
        g_failed++;
        g_test_done = true;
        co_return;
    }

    if (!listener.listen(128)) {
        g_failed++;
        g_test_done = true;
        co_return;
    }

    g_server_ready = true;

    Host clientHost;
    auto acceptResult = co_await listener.accept(&clientHost);
    if (!acceptResult) {
        g_failed++;
        g_test_done = true;
        co_return;
    }

    TcpSocket client(acceptResult.value());
    client.option().handleNonBlock();

    auto handleResult = co_await handleClient(std::move(client), file_size);
    if (!handleResult) {
        g_failed++;
        g_test_done = true;
        co_return;
    }
    co_await listener.close();
    g_test_done = true;
}

// 客户端
Task<void> client() {
    while (!g_server_ready.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    TcpSocket socket;
    socket.option().handleNonBlock();

    Host serverHost(IPType::IPV4, "127.0.0.1", TEST_PORT);
    auto connectResult = co_await socket.connect(serverHost);
    if (!connectResult) {
        LogError("Connect failed");
        g_failed++;
        co_return;
    }

    // 接收文件大小
    uint64_t file_size = 0;
    char size_buf[sizeof(file_size)];
    auto recvResult = co_await socket.recv(size_buf, sizeof(size_buf));
    if (!recvResult) {
        g_failed++;
        co_await socket.close();
        co_return;
    }
    std::memcpy(&file_size, size_buf, sizeof(file_size));

    // 接收文件
    std::ofstream ofs(RECEIVED_FILE, std::ios::binary);
    size_t total_received = 0;
    char buffer[8192];

    while (total_received < file_size) {
        size_t to_recv = std::min(sizeof(buffer), static_cast<size_t>(file_size - total_received));
        auto result = co_await socket.recv(buffer, to_recv);

        if (!result || result.value() == 0) break;

        ofs.write(buffer, result.value());
        total_received += result.value();
    }

    ofs.close();
    co_await socket.close();

    if (total_received == file_size && verifyFile(file_size)) {
        LogInfo("✓ Client received and verified {} bytes", total_received);
        g_passed++;
    } else {
        LogError("✗ Client verification failed");
        g_failed++;
    }
}

// 运行单个测试
void runTest(size_t file_size, const char* name) {
    LogInfo("\n=== Test: {} ===", name);
    g_total++;
    g_server_ready = false;
    g_test_done = false;

    createTestFile(file_size);

    IOSchedulerType scheduler;
    scheduler.start();

    scheduleTask(scheduler, server(file_size));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    scheduleTask(scheduler, client());

    while (!g_test_done.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    scheduler.stop();
    std::remove(TEST_FILE);
    std::remove(RECEIVED_FILE);
}

int main() {
    LogInfo("=== SendFile Basic Functionality Test ===\n");

    // 测试1: 小文件 (1KB)
    runTest(1024, "Small File (1KB)");

    // 测试2: 中等文件 (1MB)
    runTest(1024 * 1024, "Medium File (1MB)");

    // 测试3: 大文件 (10MB)
    runTest(10 * 1024 * 1024, "Large File (10MB)");

    // 测试4: 超大文件 (100MB)
    runTest(100 * 1024 * 1024, "Very Large File (100MB)");

    LogInfo("\n========================================");
    LogInfo("Test Summary:");
    LogInfo("  Total:  {}", g_total.load());
    LogInfo("  Passed: {}", g_passed.load());
    LogInfo("  Failed: {}", g_failed.load());
    LogInfo("========================================");

    galay::test::TestResultWriter writer("test_sendfile_basic");
    for (int i = 0; i < g_total.load(); ++i) {
        writer.addTest();
    }
    for (int i = 0; i < g_passed.load(); ++i) {
        writer.addPassed();
    }
    for (int i = 0; i < g_failed.load(); ++i) {
        writer.addFailed();
    }
    writer.writeResult();

    return (g_failed.load() == 0) ? 0 : 1;
}
