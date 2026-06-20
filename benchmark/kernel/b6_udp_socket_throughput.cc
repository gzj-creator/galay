/**
 * @file b6_udp.cc
 * @brief 用途：执行 UDP 自闭环压测，统计 runtime 模型下的整体收发性能。
 * 关键覆盖点：同进程 server/client 协作、吞吐与字节统计、完成同步与收尾。
 * 通过条件：预热与正式压测都能完成，输出结果且进程无异常退出。
 */

#include <iostream>
#include <cstring>
#include <atomic>
#include <chrono>
#include <vector>
#include "benchmark/benchmark_sync.h"
#include <galay/cpp/galay-kernel/async/udp_socket.h>
#include <galay/cpp/galay-kernel/core/task.h>
#include "test/stdout_log.h"

#ifdef USE_KQUEUE
#include <galay/cpp/galay-kernel/core/kqueue_scheduler.h>
#endif

#ifdef USE_EPOLL
#include <galay/cpp/galay-kernel/core/epoll_scheduler.h>
#endif

#ifdef USE_IOURING
#include <galay/cpp/galay-kernel/core/uring_scheduler.h>
#endif

using namespace galay::async;
using namespace galay::kernel;

// 全局统计
std::atomic<uint64_t> g_total_sent{0};
std::atomic<uint64_t> g_total_received{0};
std::atomic<uint64_t> g_total_bytes_sent{0};
std::atomic<uint64_t> g_total_bytes_received{0};
std::atomic<bool> g_running{true};
std::atomic<uint32_t> g_server_workers_ready{0};
std::atomic<bool> g_server_ready{false};

// 配置参数
constexpr int NUM_CLIENTS = 100;           // 并发客户端数量
constexpr int MESSAGES_PER_CLIENT = 1000;  // 每个客户端发送的消息数
constexpr int MESSAGE_SIZE = 256;          // 消息大小（字节）- 与TCP压测一致
constexpr int TEST_DURATION_SEC = 5;       // 测试持续时间（秒）
constexpr int NUM_SERVER_WORKERS = 4;      // 服务器工作协程数量
constexpr auto CLIENT_RECV_TIMEOUT = std::chrono::milliseconds(50);
constexpr auto CLIENT_DRAIN_TIMEOUT = std::chrono::milliseconds(250);

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

    Host bindHost(IPType::IPV4, "127.0.0.1", 9090);
    auto bindResult = socket.bind(bindHost);
    if (!bindResult) {
        LogError("Worker {}: Failed to bind", worker_id);
        co_return;
    }

    if (g_server_workers_ready.fetch_add(1, std::memory_order_acq_rel) + 1 ==
        NUM_SERVER_WORKERS) {
        g_server_ready.store(true, std::memory_order_release);
    }

    if (worker_id == 0) {
        LogInfo("UDP Server workers started on 127.0.0.1:9090");
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

// UDP客户端协程 - 流水线模式
Task<void> udpBenchmarkClient(int client_id) {
    UdpSocket socket;

    socket.option().handleNonBlock();

    // 设置发送缓冲区大小
    int send_buf_size = 2 * 1024 * 1024; // 2MB
    setsockopt(socket.handle().fd, SOL_SOCKET, SO_SNDBUF,
               &send_buf_size, sizeof(send_buf_size));

    Host serverHost(IPType::IPV4, "127.0.0.1", 9090);

    // 准备测试数据
    std::vector<char> message(MESSAGE_SIZE);
    snprintf(message.data(), MESSAGE_SIZE, "Client-%d-Message", client_id);

    char recv_buffer[65536];
    uint64_t local_sent = 0;
    uint64_t local_received = 0;

    // 流水线模式：先发送一批，再接收一批
    constexpr int PIPELINE_SIZE = 10;  // 流水线深度

    for (int batch = 0; batch < MESSAGES_PER_CLIENT / PIPELINE_SIZE && g_running.load(std::memory_order_relaxed); ++batch) {
        // 批量发送
        for (int i = 0; i < PIPELINE_SIZE; ++i) {
            auto sendResult = co_await socket.sendto(message.data(), MESSAGE_SIZE, serverHost);
            if (sendResult) {
                local_sent++;
            }
        }

        // 批量接收
        // UDP 丢包时也要保证 benchmark 可以收敛退出，不要永久卡在 recvfrom。
        for (int i = 0; i < PIPELINE_SIZE; ++i) {
            if (!g_running.load(std::memory_order_relaxed)) {
                break;
            }
            Host from;
            auto recvResult = co_await socket.recvfrom(recv_buffer, sizeof(recv_buffer), &from)
                                    .timeout(CLIENT_RECV_TIMEOUT);
            if (recvResult) {
                local_received++;
            }
        }
    }

    // 允许客户端在退出前短暂回收尾部回包，避免把轻微调度抖动误判成永久丢包。
    const auto drain_deadline = std::chrono::steady_clock::now() + CLIENT_DRAIN_TIMEOUT;
    while (local_received < local_sent &&
           std::chrono::steady_clock::now() < drain_deadline) {
        Host from;
        auto recvResult = co_await socket.recvfrom(recv_buffer, sizeof(recv_buffer), &from)
                                .timeout(CLIENT_RECV_TIMEOUT);
        if (!recvResult) {
            break;
        }
        local_received++;
    }

    g_total_sent.fetch_add(local_sent, std::memory_order_relaxed);
    g_total_received.fetch_add(local_received, std::memory_order_relaxed);
    g_total_bytes_sent.fetch_add(local_sent * MESSAGE_SIZE, std::memory_order_relaxed);
    g_total_bytes_received.fetch_add(local_received * MESSAGE_SIZE, std::memory_order_relaxed);

    co_await socket.close();
    co_return;
}

void printBenchmarkResults(std::chrono::steady_clock::time_point start_time) {
    auto end_time = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();
    double duration_sec = duration / 1000.0;

    uint64_t total_sent = g_total_sent.load();
    uint64_t total_received = g_total_received.load();
    uint64_t total_bytes_sent = g_total_bytes_sent.load();
    uint64_t total_bytes_received = g_total_bytes_received.load();

    LogInfo("\n========== UDP Benchmark Results (Optimized) ==========");
    LogInfo("Test Duration: {:.2f} seconds", duration_sec);
    LogInfo("Concurrent Clients: {}", NUM_CLIENTS);
    LogInfo("Server Workers: {}", NUM_SERVER_WORKERS);
    LogInfo("Messages per Client: {}", MESSAGES_PER_CLIENT);
    LogInfo("Message Size: {} bytes", MESSAGE_SIZE);
    LogInfo("");
    LogInfo("Total Packets Sent: {}", total_sent);
    LogInfo("Total Packets Received: {}", total_received);
    LogInfo("Packet Loss Rate: {:.2f}%",
            total_sent > 0 ? (1.0 - (double)total_received / total_sent) * 100.0 : 0.0);
    LogInfo("");
    LogInfo("Total Data Sent: {:.2f} MB", total_bytes_sent / 1024.0 / 1024.0);
    LogInfo("Total Data Received: {:.2f} MB", total_bytes_received / 1024.0 / 1024.0);
    LogInfo("");
    LogInfo("Average Throughput:");
    LogInfo("  Sent: {:.2f} pkt/s ({:.2f} MB/s)",
            total_sent / duration_sec,
            total_bytes_sent / duration_sec / 1024.0 / 1024.0);
    LogInfo("  Received: {:.2f} pkt/s ({:.2f} MB/s)",
            total_received / duration_sec,
            total_bytes_received / duration_sec / 1024.0 / 1024.0);
    LogInfo("=======================================================\n");
}

int main() {
    LogInfo("UDP Socket Benchmark Test (Optimized)");
    LogInfo("Configuration: {} clients, {} workers, {} messages/client, {} bytes/message",
            NUM_CLIENTS, NUM_SERVER_WORKERS, MESSAGES_PER_CLIENT, MESSAGE_SIZE);

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

    scheduler.start();
    LogInfo("Scheduler started");

    auto start_time = std::chrono::steady_clock::now();
    g_running.store(true, std::memory_order_relaxed);
    g_total_sent.store(0, std::memory_order_relaxed);
    g_total_received.store(0, std::memory_order_relaxed);
    g_total_bytes_sent.store(0, std::memory_order_relaxed);
    g_total_bytes_received.store(0, std::memory_order_relaxed);
    g_server_workers_ready.store(0, std::memory_order_relaxed);
    g_server_ready.store(false, std::memory_order_relaxed);

    // 启动多个服务器工作协程
    for (int i = 0; i < NUM_SERVER_WORKERS; ++i) {
        scheduleTask(scheduler, udpServerWorker(i));
    }
    LogInfo("Started {} server workers", NUM_SERVER_WORKERS);

    if (!galay::benchmark::waitForFlag(g_server_ready, std::chrono::seconds(2))) {
        LogError("Server workers did not become ready before client start");
        g_running.store(false, std::memory_order_relaxed);
        scheduler.stop();
        return 1;
    }

    // 启动多个客户端
    LogInfo("Starting {} clients...", NUM_CLIENTS);
    for (int i = 0; i < NUM_CLIENTS; ++i) {
        scheduleTask(scheduler, udpBenchmarkClient(i));
    }

    // 运行测试
    LogInfo("Benchmark running for {} seconds...", TEST_DURATION_SEC);
    std::this_thread::sleep_for(std::chrono::seconds(TEST_DURATION_SEC));

    // 停止测试
    g_running.store(false, std::memory_order_relaxed);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    scheduler.stop();
    LogInfo("Scheduler stopped");

    // 打印结果
    printBenchmarkResults(start_time);

    return 0;
}
