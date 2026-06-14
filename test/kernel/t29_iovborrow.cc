/**
 * @file t29_iovborrow.cc
 * @brief 用途：验证 `readv` 数组借用视图不会破坏原始缓冲区布局。
 * 关键覆盖点：借用数组映射、缓冲区指针与长度保持、只读视图一致性。
 * 通过条件：借用结果与原始数组完全匹配，测试返回 0。
 */

#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>

#include "kernel/async/tcp_socket.h"
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

namespace {

std::atomic<bool> g_server_ready{false};
std::atomic<bool> g_test_passed{false};

Task<void> borrowedServer([[maybe_unused]] IOScheduler* scheduler) {
    TcpSocket listener;

    auto opt = listener.option().handleReuseAddr();
    if (!opt) {
        LogError("[Server] reuse addr failed: {}", opt.error().message());
        co_return;
    }

    opt = listener.option().handleNonBlock();
    if (!opt) {
        LogError("[Server] non-block failed: {}", opt.error().message());
        co_return;
    }

    Host bindHost(IPType::IPV4, "127.0.0.1", 9091);
    auto bindResult = listener.bind(bindHost);
    if (!bindResult) {
        LogError("[Server] bind failed: {}", bindResult.error().message());
        co_return;
    }

    auto listenResult = listener.listen(16);
    if (!listenResult) {
        LogError("[Server] listen failed: {}", listenResult.error().message());
        co_return;
    }

    g_server_ready.store(true, std::memory_order_release);

    Host clientHost;
    auto acceptResult = co_await listener.accept(&clientHost);
    if (!acceptResult) {
        LogError("[Server] accept failed: {}", acceptResult.error().message());
        co_await listener.close();
        co_return;
    }

    TcpSocket client(acceptResult.value());
    client.option().handleNonBlock();

    char header[16]{};
    char body[32]{};
    std::array<struct iovec, 2> recvIovecs{};
    recvIovecs[0].iov_base = header;
    recvIovecs[0].iov_len = sizeof(header);
    recvIovecs[1].iov_base = body;
    recvIovecs[1].iov_len = sizeof(body);

    auto readvResult = co_await client.readv(recvIovecs, 2);
    if (!readvResult) {
        LogError("[Server] readv failed: {}", readvResult.error().message());
        co_await client.close();
        co_await listener.close();
        co_return;
    }

    const char* respHeader = "RESP:OK_________";
    const char* respBody = "array-borrowed";
    std::array<struct iovec, 2> sendIovecs{};
    sendIovecs[0].iov_base = const_cast<char*>(respHeader);
    sendIovecs[0].iov_len = 16;
    sendIovecs[1].iov_base = const_cast<char*>(respBody);
    sendIovecs[1].iov_len = std::strlen(respBody);

    auto writevResult = co_await client.writev(sendIovecs, 2);
    if (!writevResult) {
        LogError("[Server] writev failed: {}", writevResult.error().message());
        co_await client.close();
        co_await listener.close();
        co_return;
    }

    const bool headerOk = std::strncmp(header, "HEADER:borrowed", 15) == 0;
    const bool bodyOk = std::strncmp(body, "BODY:payload", 12) == 0;
    g_test_passed.store(headerOk && bodyOk, std::memory_order_release);

    co_await client.close();
    co_await listener.close();
    co_return;
}

Task<void> borrowedClient([[maybe_unused]] IOScheduler* scheduler) {
    while (!g_server_ready.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    TcpSocket client;
    auto opt = client.option().handleNonBlock();
    if (!opt) {
        LogError("[Client] non-block failed: {}", opt.error().message());
        co_return;
    }

    Host serverHost(IPType::IPV4, "127.0.0.1", 9091);
    auto connectResult = co_await client.connect(serverHost);
    if (!connectResult) {
        LogError("[Client] connect failed: {}", connectResult.error().message());
        co_return;
    }

    char header[] = "HEADER:borrowed";
    char body[] = "BODY:payload";
    std::array<struct iovec, 2> sendIovecs{};
    sendIovecs[0].iov_base = header;
    sendIovecs[0].iov_len = 16;
    sendIovecs[1].iov_base = body;
    sendIovecs[1].iov_len = 12;

    auto writevResult = co_await client.writev(sendIovecs, 2);
    if (!writevResult) {
        LogError("[Client] writev failed: {}", writevResult.error().message());
        co_await client.close();
        co_return;
    }

    char respHeader[16]{};
    char respBody[32]{};
    std::array<struct iovec, 2> recvIovecs{};
    recvIovecs[0].iov_base = respHeader;
    recvIovecs[0].iov_len = sizeof(respHeader);
    recvIovecs[1].iov_base = respBody;
    recvIovecs[1].iov_len = sizeof(respBody);

    auto readvResult = co_await client.readv(recvIovecs, 2);
    if (!readvResult) {
        LogError("[Client] readv failed: {}", readvResult.error().message());
        co_await client.close();
        co_return;
    }

    co_await client.close();
    co_return;
}

}  // namespace

int main() {
    LogInfo("=== T29: Readv array borrowed overload ===");

#ifdef USE_KQUEUE
    KqueueScheduler scheduler;
#elif defined(USE_EPOLL)
    EpollScheduler scheduler;
#elif defined(USE_IOURING)
    IOUringScheduler scheduler;
#else
    LogWarn("No supported scheduler available");
    return 1;
#endif

    scheduler.start();
    scheduleTask(scheduler, borrowedServer(&scheduler));
    scheduleTask(scheduler, borrowedClient(&scheduler));

    std::this_thread::sleep_for(std::chrono::seconds(3));
    scheduler.stop();

    if (!g_test_passed.load(std::memory_order_acquire)) {
        LogError("T29 failed");
        return 1;
    }

    LogInfo("T29 passed");
    return 0;
}
