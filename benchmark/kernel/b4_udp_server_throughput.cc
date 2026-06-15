/**
 * @file b4_udp.cc
 * @brief 用途：作为 UDP 压测服务端，承接高并发报文收发负载。
 * 关键覆盖点：端口绑定、报文接收与回发、多 worker 协作和字节统计。
 * 通过条件：服务端可持续响应压测流量并输出统计，停止后干净退出。
 */

#include <iostream>
#include <cstring>
#include <atomic>
#include <chrono>
#include <csignal>
#include <thread>
#include "galay-kernel/async/udp_socket.h"
#include "galay-kernel/core/task.h"
#include "test/stdout_log.h"

#ifdef USE_KQUEUE
#include "galay-kernel/core/kqueue_scheduler.h"
#endif

#ifdef USE_EPOLL
#include "galay-kernel/core/epoll_scheduler.h"
#endif

#ifdef USE_IOURING
#include "galay-kernel/core/uring_scheduler.h"
#endif

using namespace galay::async;
using namespace galay::kernel;

// 全局统计
std::atomic<uint64_t> g_total_sent{0};
std::atomic<uint64_t> g_total_received{0};
std::atomic<uint64_t> g_total_bytes_sent{0};
std::atomic<uint64_t> g_total_bytes_received{0};
std::atomic<bool> g_running{true};

// 配置参数
constexpr int NUM_SERVER_WORKERS = 4;      // 服务器工作协程数量
constexpr int SERVER_PORT = 9090;          // 服务器端口

// 全局调度器指针，用于信号处理
IOScheduler* g_scheduler = nullptr;

// 信号处理函数
void signalHandler(int signum) {
    LogInfo("\nReceived signal {}, shutting down server...", signum);
    g_running.store(false, std::memory_order_relaxed);
}

// UDP Echo服务器工作协程 - 多协程并发处理
Task<void> udpServerWorker(int worker_id) {
    UdpSocket socket;

    socket.option().handleReuseAddr();
    socket.option().handleReusePort();  // 关键：允许多个socket绑定同一端口
    socket.option().handleNonBlock();

    // 设置接收缓冲区大小
    int recv_buf_size = 8 * 1024 * 1024; // 8MB
    setsockopt(socket.handle().fd, SOL_SOCKET, SO_RCVBUF,
               &recv_buf_size, sizeof(recv_buf_size));

    Host bindHost(IPType::IPV4, "0.0.0.0", SERVER_PORT);
    auto bindResult = socket.bind(bindHost);
    if (!bindResult) {
        LogError("Worker {}: Failed to bind", worker_id);
        co_return;
    }

    if (worker_id == 0) {
        LogInfo("UDP Server workers started on 0.0.0.0:{}", SERVER_PORT);
    }

    char buffer[65536];
    while (g_running.load(std::memory_order_relaxed)) {
        Host from;
        auto recvResult = co_await socket.recvfrom(buffer, sizeof(buffer), &from);

        if (!recvResult) {
            if (recvResult.error().code() == EAGAIN ||
                recvResult.error().code() == EWOULDBLOCK) {
                continue;
            }
            break;
        }

        size_t bytes = recvResult.value();
        g_total_received.fetch_add(1, std::memory_order_relaxed);
        g_total_bytes_received.fetch_add(bytes, std::memory_order_relaxed);

        // Echo回发送方
        auto sendResult = co_await socket.sendto(buffer, bytes, from);
        if (sendResult) {
            g_total_sent.fetch_add(1, std::memory_order_relaxed);
            g_total_bytes_sent.fetch_add(sendResult.value(), std::memory_order_relaxed);
        }
    }

    co_await socket.close();
    co_return;
}

// 统计打印线程，避免阻塞单线程调度器的事件循环。
void statsReporter() {
    auto last_time = std::chrono::steady_clock::now();
    uint64_t last_received = 0;
    uint64_t last_sent = 0;
    uint64_t last_bytes_received = 0;
    uint64_t last_bytes_sent = 0;

    while (g_running.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::seconds(1));

        auto now = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_time).count() / 1000.0;

        uint64_t current_received = g_total_received.load();
        uint64_t current_sent = g_total_sent.load();
        uint64_t current_bytes_received = g_total_bytes_received.load();
        uint64_t current_bytes_sent = g_total_bytes_sent.load();

        uint64_t delta_received = current_received - last_received;
        uint64_t delta_sent = current_sent - last_sent;
        uint64_t delta_bytes_received = current_bytes_received - last_bytes_received;
        uint64_t delta_bytes_sent = current_bytes_sent - last_bytes_sent;

        LogInfo("Stats: Recv {:.2f} pkt/s ({:.2f} MB/s) | Send {:.2f} pkt/s ({:.2f} MB/s) | Total: {} recv, {} sent",
                delta_received / duration,
                delta_bytes_received / duration / 1024.0 / 1024.0,
                delta_sent / duration,
                delta_bytes_sent / duration / 1024.0 / 1024.0,
                current_received,
                current_sent);

        last_time = now;
        last_received = current_received;
        last_sent = current_sent;
        last_bytes_received = current_bytes_received;
        last_bytes_sent = current_bytes_sent;
    }

}

int main() {
    LogInfo("UDP Echo Server (Benchmark Mode)");
    LogInfo("Configuration: {} workers, port {}", NUM_SERVER_WORKERS, SERVER_PORT);

    // 注册信号处理
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

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
    LogError("This benchmark requires kqueue (macOS), epoll or io_uring (Linux)");
    return 1;
#endif

    g_scheduler = &scheduler;
    scheduler.start();
    LogInfo("Scheduler started");

    // 启动多个服务器工作协程
    for (int i = 0; i < NUM_SERVER_WORKERS; ++i) {
        scheduleTask(scheduler, udpServerWorker(i));
    }
    LogInfo("Started {} server workers", NUM_SERVER_WORKERS);

    // 周期统计放到普通线程上，避免阻塞 scheduler 线程。
    std::thread reporter(statsReporter);

    LogInfo("Server is running. Press Ctrl+C to stop.");

    // 等待停止信号
    while (g_running.load(std::memory_order_relaxed)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // 停止调度器
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    scheduler.stop();
    if (reporter.joinable()) {
        reporter.join();
    }
    LogInfo("Scheduler stopped");

    // 打印最终统计
    LogInfo("\n========== Final Statistics ==========");
    LogInfo("Total Packets Received: {}", g_total_received.load());
    LogInfo("Total Packets Sent: {}", g_total_sent.load());
    LogInfo("Total Data Received: {:.2f} MB", g_total_bytes_received.load() / 1024.0 / 1024.0);
    LogInfo("Total Data Sent: {:.2f} MB", g_total_bytes_sent.load() / 1024.0 / 1024.0);
    LogInfo("======================================\n");

    return 0;
}
