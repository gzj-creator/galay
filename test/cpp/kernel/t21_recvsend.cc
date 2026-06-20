/**
 * @file t21_recvsend.cc
 * @brief 用途：验证并发收发场景下的多端点运行时协作与数据正确性。
 * 关键覆盖点：双向并发发送接收、独立 Runtime 配置、消息闭环与完成同步。
 * 通过条件：收发两端都完成预期工作负载，断言成立并返回 0。
 */

#include <iostream>
#include <cstring>
#include <atomic>
#include <thread>
#include <chrono>
#include <vector>
#include <sys/socket.h>
#include <galay/cpp/galay-kernel/async/tcp_socket.h>
#include <galay/cpp/galay-kernel/core/task.h>
#include "test/cpp/common/stdout_log.h"

#ifdef USE_KQUEUE
#include <galay/cpp/galay-kernel/core/kqueue_scheduler.h>
using TestScheduler = galay::kernel::KqueueScheduler;
#endif

#ifdef USE_EPOLL
#include <galay/cpp/galay-kernel/core/epoll_scheduler.h>
using TestScheduler = galay::kernel::EpollScheduler;
#endif

#ifdef USE_IOURING
#include <galay/cpp/galay-kernel/core/uring_scheduler.h>
using TestScheduler = galay::kernel::IOUringScheduler;
#endif

using namespace galay::async;
using namespace galay::kernel;

static std::atomic<bool>    g_server_ready{false};
static std::atomic<int64_t> g_server_recv_bytes{0};
static std::atomic<int>     g_server_send_rounds{0};
static std::atomic<int64_t> g_client_recv_bytes{0};
static std::atomic<int>     g_client_send_rounds{0};

constexpr int     kRounds   = 2000;
constexpr int     kPort     = 19876;
constexpr size_t  kMsgLen   = 256 * 1024;  // 256KB per message
constexpr int64_t kExpectedBytes = static_cast<int64_t>(kRounds) * kMsgLen;
constexpr size_t  kRecvBuf  = 64 * 1024;   // 64KB recv buffer
constexpr int     kSockBuf  = 64 * 1024;   // 64KB SO_SNDBUF/SO_RCVBUF — 制造背压但不至于太小

// 使用共享指针管理 socket 生命周期
static std::shared_ptr<TcpSocket> g_server_client;
static std::shared_ptr<TcpSocket> g_server_listener;
static std::shared_ptr<TcpSocket> g_client_sock;

// 构造填充数据：用可辨识的 pattern 方便调试
static std::vector<char> makeSendBuf(char tag) {
    std::vector<char> buf(kMsgLen);
    std::memset(buf.data(), tag, kMsgLen);
    return buf;
}

// 压小内核 socket 缓冲区，迫使 send 更快触发 EAGAIN
static void shrinkSocketBuf(int fd) {
    int val = kSockBuf;
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &val, sizeof(val));
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &val, sizeof(val));
}

// ==================== 通用收发协程 ====================

// recv 端：用小 buffer 接收，制造背压
Task<void> recvLoop(std::shared_ptr<TcpSocket> sock,
                    std::atomic<int64_t>& counter,
                    const char* label) {
    char buffer[kRecvBuf];
    int64_t totalBytes = 0;
    while (totalBytes < kExpectedBytes) {
        auto result = co_await sock->recv(buffer, sizeof(buffer));
        if (!result) {
            LogError("[{}] failed after {} bytes: {}", label, totalBytes, result.error().message());
            co_return;
        }
        int64_t n = static_cast<int64_t>(result.value());
        if (n == 0) {
            LogError("[{}] peer closed after {} bytes", label, totalBytes);
            co_return;
        }
        totalBytes += n;
        counter.store(totalBytes);
    }
    LogInfo("[{}] done, total {} bytes", label, totalBytes);
    co_return;
}

// send 端：每轮发 256KB，处理 partial write
Task<void> sendLoop(std::shared_ptr<TcpSocket> sock,
                    std::atomic<int>& counter,
                    const char* label,
                    char tag) {
    auto buf = makeSendBuf(tag);
    for (int i = 0; i < kRounds; ++i) {
        size_t sent = 0;
        while (sent < kMsgLen) {
            auto result = co_await sock->send(buf.data() + sent, kMsgLen - sent);
            if (!result) {
                LogError("[{}] round {} failed after {} bytes: {}",
                         label, i, sent, result.error().message());
                co_return;
            }
            sent += result.value();
        }
        counter.fetch_add(1);
    }
    LogInfo("[{}] done, {} rounds", label, kRounds);
    co_return;
}

// ==================== Server ====================

Task<void> serverMain(IOScheduler* scheduler) {
    g_server_listener = std::make_shared<TcpSocket>();
    g_server_listener->option().handleReuseAddr();
    g_server_listener->option().handleNonBlock();

    Host bindHost(IPType::IPV4, "127.0.0.1", kPort);
    auto bindResult = g_server_listener->bind(bindHost);
    if (!bindResult) {
        LogError("[Server] bind failed: {}", bindResult.error().message());
        co_return;
    }
    auto listenResult = g_server_listener->listen(128);
    if (!listenResult) {
        LogError("[Server] listen failed: {}", listenResult.error().message());
        co_return;
    }

    LogInfo("[Server] listening on 127.0.0.1:{}", kPort);
    g_server_ready.store(true);

    Host clientHost;
    auto acceptResult = co_await g_server_listener->accept(&clientHost);
    if (!acceptResult) {
        LogError("[Server] accept failed: {}", acceptResult.error().message());
        co_return;
    }

    LogInfo("[Server] client connected from {}:{}", clientHost.ip(), clientHost.port());

    g_server_client = std::make_shared<TcpSocket>(acceptResult.value());
    g_server_client->option().handleNonBlock();
    shrinkSocketBuf(g_server_client->handle().fd);

    // 关键：同一个 socket 同时提交读和写任务
    scheduleTask(scheduler, recvLoop(g_server_client, g_server_recv_bytes, "S-Recv"));
    scheduleTask(scheduler, sendLoop(g_server_client, g_server_send_rounds, "S-Send", 'S'));
    co_return;
}

// ==================== Client ====================

Task<void> clientMain(IOScheduler* scheduler) {
    while (!g_server_ready.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    g_client_sock = std::make_shared<TcpSocket>();
    g_client_sock->option().handleNonBlock();
    shrinkSocketBuf(g_client_sock->handle().fd);

    Host serverHost(IPType::IPV4, "127.0.0.1", kPort);
    auto connectResult = co_await g_client_sock->connect(serverHost);
    if (!connectResult) {
        LogError("[Client] connect failed: {}", connectResult.error().message());
        co_return;
    }

    LogInfo("[Client] connected");

    // 关键：同一个 socket 同时提交读和写任务
    scheduleTask(scheduler, recvLoop(g_client_sock, g_client_recv_bytes, "C-Recv"));
    scheduleTask(scheduler, sendLoop(g_client_sock, g_client_send_rounds, "C-Send", 'C'));
    co_return;
}

// ==================== main ====================

int main() {
    LogInfo("=== T21: Concurrent Recv+Send on same socket ===");

#if !defined(USE_KQUEUE) && !defined(USE_EPOLL) && !defined(USE_IOURING)
    LogWarn("No supported scheduler");
    return 1;
#else

#ifdef USE_KQUEUE
    LogInfo("Backend: KqueueScheduler");
#elif defined(USE_EPOLL)
    LogInfo("Backend: EpollScheduler");
#elif defined(USE_IOURING)
    LogInfo("Backend: IOUringScheduler");
#endif

    // Server 和 Client 各用独立调度器，避免单线程调度饥饿
    TestScheduler serverScheduler;
    TestScheduler clientScheduler;

    serverScheduler.start();
    clientScheduler.start();

    scheduleTask(serverScheduler, serverMain(&serverScheduler));
    scheduleTask(clientScheduler, clientMain(&clientScheduler));

    // 在 main 线程等待
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (std::chrono::steady_clock::now() < deadline) {
        if (g_server_recv_bytes.load() >= kExpectedBytes &&
            g_server_send_rounds.load() >= kRounds &&
            g_client_recv_bytes.load() >= kExpectedBytes &&
            g_client_send_rounds.load() >= kRounds)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    serverScheduler.stop();
    clientScheduler.stop();

    g_server_client.reset();
    g_server_listener.reset();
    g_client_sock.reset();

    LogInfo("=== Results ===");
    LogInfo("  Server recv bytes: {}/{}", g_server_recv_bytes.load(), kExpectedBytes);
    LogInfo("  Server send rounds: {}/{}", g_server_send_rounds.load(), kRounds);
    LogInfo("  Client recv bytes: {}/{}", g_client_recv_bytes.load(), kExpectedBytes);
    LogInfo("  Client send rounds: {}/{}", g_client_send_rounds.load(), kRounds);

    bool passed = (g_server_recv_bytes.load() >= kExpectedBytes) &&
                  (g_server_send_rounds.load() >= kRounds) &&
                  (g_client_recv_bytes.load() >= kExpectedBytes) &&
                  (g_client_send_rounds.load() >= kRounds);

    if (passed) {
        LogInfo("=== TEST PASSED ===");
        return 0;
    } else {
        LogError("=== TEST FAILED ===");
        return 1;
    }

#endif
}
