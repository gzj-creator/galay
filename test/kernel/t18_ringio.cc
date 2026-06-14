/**
 * @file t18_ringio.cc
 * @brief 用途：验证 `RingBuffer` 与网络 IO 组合使用时的数据读写闭环。
 * 关键覆盖点：环形缓冲区读写、`iovec` 借用接口、与 Socket IO 的集成路径。
 * 通过条件：缓冲区内容与网络收发结果一致，测试返回 0。
 */

#include <iostream>
#include <array>
#include <cstring>
#include <cassert>
#include <atomic>
#include "kernel/async/tcp_socket.h"
#include "kernel/common/buffer.h"
#include "kernel/kernel/task.h"
#include "test/stdout_log.h"

#ifdef USE_KQUEUE
#include "kernel/kernel/kqueue_scheduler.h"
#endif

#ifdef USE_EPOLL
#include "kernel/kernel/epoll_scheduler.h"
#endif

#ifdef USE_IOURING
#include "kernel/kernel/uring_scheduler.h"
#endif

using namespace galay::async;
using namespace galay::kernel;

std::atomic<bool> g_server_ready{false};
std::atomic<bool> g_test_passed{false};

// ============ 单元测试 ============

std::string collectReadable(const RingBuffer& buffer) {
    std::array<struct iovec, 2> iovecs{};
    const size_t count = buffer.getReadIovecs(iovecs);
    std::string collected;
    for (size_t i = 0; i < count; ++i) {
        const auto& iov = iovecs[i];
        collected.append(static_cast<const char*>(iov.iov_base), iov.iov_len);
    }
    return collected;
}

void testBasicOperations() {
    LogInfo("=== Test: Basic Operations ===");

    RingBuffer buf(100);
    assert(buf.capacity() == 100);
    assert(buf.readable() == 0);
    assert(buf.writable() == 100);
    assert(buf.empty());
    assert(!buf.full());

    // 写入数据
    const char* data = "Hello World";
    size_t written = buf.write(data, strlen(data));
    assert(written == 11);
    assert(buf.readable() == 11);
    assert(buf.writable() == 89);

    // 消费部分数据
    buf.consume(6);
    assert(buf.readable() == 5);

    // 清空
    buf.clear();
    assert(buf.empty());
    assert(buf.readable() == 0);

    LogInfo("Test: Basic Operations PASSED");
}

void testWrapAround() {
    LogInfo("=== Test: Wrap Around ===");

    RingBuffer buf(20);

    // 写入15字节
    buf.write("123456789012345", 15);
    assert(buf.readable() == 15);

    // 消费10字节，readIndex=10, writeIndex=15, size=5
    buf.consume(10);
    assert(buf.readable() == 5);

    // 再写入10字节，会环绕
    // writeIndex: 15 -> 20 -> 0 -> 5
    // 数据布局: [10,15)="12345", [15,20)="ABCDE", [0,5)="FGHIJ"
    buf.write("ABCDEFGHIJ", 10);
    assert(buf.readable() == 15);

    // 验证 getReadIovecs 返回两段
    std::array<struct iovec, 2> readIovecs{};
    const size_t readCount = buf.getReadIovecs(readIovecs);
    assert(readCount == 2);

    // 第一段: [10, 20) = "12345ABCDE" (10字节)
    assert(readIovecs[0].iov_len == 10);
    assert(std::memcmp(readIovecs[0].iov_base, "12345ABCDE", 10) == 0);

    // 第二段: [0, 5) = "FGHIJ" (5字节)
    assert(readIovecs[1].iov_len == 5);
    assert(std::memcmp(readIovecs[1].iov_base, "FGHIJ", 5) == 0);

    LogInfo("Test: Wrap Around PASSED");
}

void testGetWriteIovecs() {
    LogInfo("=== Test: getWriteIovecs ===");

    RingBuffer buf(20);

    // 空缓冲区，应该返回一段 [0, 20)
    std::array<struct iovec, 2> iovecs1{};
    const size_t iovecs1Count = buf.getWriteIovecs(iovecs1);
    assert(iovecs1Count == 1);
    assert(iovecs1[0].iov_len == 20);

    // 写入10字节
    buf.write("0123456789", 10);

    // 应该返回一段 [10, 20)
    std::array<struct iovec, 2> iovecs2{};
    const size_t iovecs2Count = buf.getWriteIovecs(iovecs2);
    assert(iovecs2Count == 1);
    assert(iovecs2[0].iov_len == 10);

    // 消费5字节，readIndex=5
    buf.consume(5);

    // 应该返回两段 [10, 20) 和 [0, 5)
    std::array<struct iovec, 2> iovecs3{};
    const size_t iovecs3Count = buf.getWriteIovecs(iovecs3);
    assert(iovecs3Count == 2);
    assert(iovecs3[0].iov_len == 10);
    assert(iovecs3[1].iov_len == 5);

    LogInfo("Test: getWriteIovecs PASSED");
}

void testGetReadIovecs() {
    LogInfo("=== Test: getReadIovecs ===");

    RingBuffer buf(20);

    // 空缓冲区
    std::array<struct iovec, 2> iovecs1{};
    const size_t iovecs1Count = buf.getReadIovecs(iovecs1);
    assert(iovecs1Count == 0);

    // 写入数据（连续）
    buf.write("Hello", 5);
    std::array<struct iovec, 2> iovecs2{};
    const size_t iovecs2Count = buf.getReadIovecs(iovecs2);
    assert(iovecs2Count == 1);
    assert(iovecs2[0].iov_len == 5);

    // 制造环绕情况
    buf.consume(5);
    buf.write("12345678901234567890", 20); // 写满
    buf.consume(15); // 消费15字节
    buf.write("ABCDE", 5); // 环绕写入

    std::array<struct iovec, 2> iovecs3{};
    const size_t iovecs3Count = buf.getReadIovecs(iovecs3);
    assert(iovecs3Count == 2);

    LogInfo("Test: getReadIovecs PASSED");
}

void testFullAndEmpty() {
    LogInfo("=== Test: Full and Empty ===");

    RingBuffer buf(10);

    assert(buf.empty());
    assert(!buf.full());

    buf.write("1234567890", 10);
    assert(!buf.empty());
    assert(buf.full());
    assert(buf.writable() == 0);

    // 满时 getWriteIovecs 应该返回空
    std::array<struct iovec, 2> iovecs{};
    const size_t iovecCount = buf.getWriteIovecs(iovecs);
    assert(iovecCount == 0);

    buf.consume(10);
    assert(buf.empty());
    assert(!buf.full());

    LogInfo("Test: Full and Empty PASSED");
}

void testMoveSemantics() {
    LogInfo("=== Test: Move Semantics ===");

    RingBuffer buf1(100);
    buf1.write("Test Data", 9);

    // 移动构造
    RingBuffer buf2(std::move(buf1));
    assert(buf2.readable() == 9);
    assert(buf2.capacity() == 100);

    // 移动赋值
    RingBuffer buf3(50);
    buf3 = std::move(buf2);
    assert(buf3.readable() == 9);
    assert(buf3.capacity() == 100);

    LogInfo("Test: Move Semantics PASSED");
}

// ============ 集成测试（网络 IO）============

// 服务器协程 - 使用 RingBuffer + readv 接收数据
Task<void> ringBufferServer([[maybe_unused]] IOScheduler* scheduler) {
    LogInfo("[Server] Starting...");
    TcpSocket listener;

    listener.option().handleReuseAddr();
    listener.option().handleNonBlock();

    Host bindHost(IPType::IPV4, "127.0.0.1", 9091);
    auto bindResult = listener.bind(bindHost);
    if (!bindResult) {
        LogError("[Server] Failed to bind: {}", bindResult.error().message());
        co_return;
    }

    auto listenResult = listener.listen(128);
    if (!listenResult) {
        LogError("[Server] Failed to listen: {}", listenResult.error().message());
        co_return;
    }

    LogInfo("[Server] Listening on 127.0.0.1:9091");
    g_server_ready = true;

    Host clientHost;
    auto acceptResult = co_await listener.accept(&clientHost);
    if (!acceptResult) {
        LogError("[Server] Failed to accept: {}", acceptResult.error().message());
        co_return;
    }

    LogInfo("[Server] Client connected from {}:{}", clientHost.ip(), clientHost.port());

    TcpSocket client(acceptResult.value());
    client.option().handleNonBlock();

    // 使用 RingBuffer 接收数据
    RingBuffer recvBuffer(1024);
    std::array<struct iovec, 2> recvWriteIovecs{};
    std::array<struct iovec, 2> sendReadIovecs{};

    // 获取可写 iovec 用于 readv
    size_t recvWriteCount = recvBuffer.getWriteIovecs(recvWriteIovecs);
    LogInfo("[Server] Prepared {} iovecs for readv", recvWriteCount);

    auto readvResult = co_await client.readv(recvWriteIovecs, recvWriteCount);
    if (!readvResult) {
        LogError("[Server] readv failed: {}", readvResult.error().message());
        co_await client.close();
        co_await listener.close();
        co_return;
    }

    size_t bytesRead = readvResult.value();
    recvBuffer.produce(bytesRead);
    LogInfo("[Server] readv received {} bytes, readable: {}", bytesRead, recvBuffer.readable());

    // 获取可读数据验证
    const std::string received = collectReadable(recvBuffer);
    LogInfo("[Server] Received data: '{}'", received);

    bool dataOk = (received.find("Hello from RingBuffer client!") != std::string::npos);
    if (dataOk) {
        LogInfo("[Server] Data verification PASSED!");
        g_test_passed = true;
    } else {
        LogError("[Server] Data verification FAILED!");
    }

    // 使用 RingBuffer + writev 发送响应
    RingBuffer sendBuffer(1024);
    std::string response = "Response: " + received + " [echoed]";
    sendBuffer.write(response.data(), response.size());

    LogInfo("[Server] Sending {} bytes via writev", sendBuffer.readable());

    size_t sendReadCount = sendBuffer.getReadIovecs(sendReadIovecs);
    if (sendReadCount != 0) {
        auto writevResult = co_await client.writev(sendReadIovecs, sendReadCount);
        if (!writevResult) {
            LogError("[Server] writev failed: {}", writevResult.error().message());
        } else {
            sendBuffer.consume(writevResult.value());
            LogInfo("[Server] writev sent {} bytes", writevResult.value());
        }
    }

    co_await client.close();
    co_await listener.close();
    LogInfo("[Server] Stopped");
    co_return;
}

// 客户端协程 - 使用 RingBuffer + writev 发送数据
Task<void> ringBufferClient([[maybe_unused]] IOScheduler* scheduler) {
    // 等待服务器就绪
    while (!g_server_ready) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    LogInfo("[Client] Starting...");
    TcpSocket client;
    client.option().handleNonBlock();

    Host serverHost(IPType::IPV4, "127.0.0.1", 9091);
    auto connectResult = co_await client.connect(serverHost);
    if (!connectResult) {
        LogError("[Client] Failed to connect: {}", connectResult.error().message());
        co_return;
    }

    LogInfo("[Client] Connected to server");

    // 使用 RingBuffer 准备发送数据
    RingBuffer sendBuffer(1024);
    const char* msg = "Hello from RingBuffer client!";
    sendBuffer.write(msg, strlen(msg));
    std::array<struct iovec, 2> sendReadIovecs{};
    std::array<struct iovec, 2> recvWriteIovecs{};

    LogInfo("[Client] Sending {} bytes via writev", sendBuffer.readable());

    // 使用 writev 发送
    size_t sendReadCount = sendBuffer.getReadIovecs(sendReadIovecs);
    auto writevResult = co_await client.writev(sendReadIovecs, sendReadCount);
    if (!writevResult) {
        LogError("[Client] writev failed: {}", writevResult.error().message());
        co_await client.close();
        co_return;
    }

    sendBuffer.consume(writevResult.value());
    LogInfo("[Client] writev sent {} bytes", writevResult.value());

    // 使用 RingBuffer + readv 接收响应
    RingBuffer recvBuffer(1024);

    size_t recvWriteCount = recvBuffer.getWriteIovecs(recvWriteIovecs);
    auto readvResult = co_await client.readv(recvWriteIovecs, recvWriteCount);
    if (!readvResult) {
        LogError("[Client] readv failed: {}", readvResult.error().message());
    } else {
        recvBuffer.produce(readvResult.value());

        // 读取响应内容
        const std::string response = collectReadable(recvBuffer);
        LogInfo("[Client] Received {} bytes: '{}'", recvBuffer.readable(), response);
    }

    co_await client.close();
    LogInfo("[Client] Stopped");
    co_return;
}

int main() {
    LogInfo("=== RingBuffer Unit Tests ===");

    // 运行单元测试
    testBasicOperations();
    testWrapAround();
    testGetWriteIovecs();
    testGetReadIovecs();
    testFullAndEmpty();
    testMoveSemantics();

    LogInfo("=== All Unit Tests PASSED ===");
    LogInfo("");
    LogInfo("=== RingBuffer + readv/writev Integration Test ===");

#ifdef USE_KQUEUE
    LogInfo("Using KqueueScheduler (macOS)");
    KqueueScheduler scheduler;
#elif defined(USE_EPOLL)
    LogInfo("Using EpollScheduler (Linux)");
    EpollScheduler scheduler;
#elif defined(USE_IOURING)
    LogInfo("Using IOUringScheduler (Linux io_uring)");
    IOUringScheduler scheduler;
#else
    LogWarn("No supported scheduler available, skipping integration test");
    return 0;
#endif

    scheduler.start();

    // 启动服务器
    scheduleTask(scheduler, ringBufferServer(&scheduler));

    // 启动客户端
    scheduleTask(scheduler, ringBufferClient(&scheduler));

    // 等待测试完成
    std::this_thread::sleep_for(std::chrono::seconds(3));

    scheduler.stop();

    if (g_test_passed) {
        LogInfo("=== ALL TESTS PASSED ===");
        return 0;
    } else {
        LogError("=== INTEGRATION TEST FAILED ===");
        return 1;
    }
}
