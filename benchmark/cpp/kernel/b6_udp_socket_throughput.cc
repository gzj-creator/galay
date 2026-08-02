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
#include <limits>
#include <vector>
#include "benchmark/cpp/common/benchmark_sync.h"
#include <galay/cpp/galay-kernel/async/async_udp.h>
#include <galay/cpp/galay-kernel/core/task.h>
#include "test/cpp/common/stdout_log.h"

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
std::atomic<uint64_t> g_client_sent{0};
std::atomic<uint64_t> g_client_received{0};
std::atomic<uint64_t> g_client_bytes_sent{0};
std::atomic<uint64_t> g_client_bytes_received{0};
std::atomic<uint64_t> g_server_received{0};
std::atomic<uint64_t> g_server_sent{0};
std::atomic<uint64_t> g_server_bytes_received{0};
std::atomic<uint64_t> g_server_bytes_sent{0};
std::atomic<uint64_t> g_errors{0};
std::atomic<bool> g_running{true};
std::atomic<uint32_t> g_server_workers_ready{0};
std::atomic<bool> g_server_ready{false};
galay::benchmark::CompletionLatch* g_client_completion = nullptr;
galay::benchmark::CompletionLatch* g_server_completion = nullptr;

// 配置参数
constexpr int NUM_CLIENTS = 100;           // 并发客户端数量
constexpr int MESSAGE_SIZE = 256;          // 消息大小（字节）- 与TCP压测一致
constexpr int TEST_DURATION_SEC = 5;       // 测试持续时间（秒）
constexpr int NUM_SERVER_WORKERS = 4;      // 服务器工作协程数量
constexpr auto CLIENT_RECV_TIMEOUT = std::chrono::milliseconds(50);
constexpr auto CLIENT_DRAIN_TIMEOUT = std::chrono::milliseconds(250);
constexpr auto SERVER_RECV_TIMEOUT = std::chrono::milliseconds(100);

struct UdpStatsSnapshot {
    uint64_t client_sent = 0;
    uint64_t client_received = 0;
    uint64_t client_bytes_sent = 0;
    uint64_t client_bytes_received = 0;
    uint64_t server_received = 0;
    uint64_t server_sent = 0;
    uint64_t server_bytes_received = 0;
    uint64_t server_bytes_sent = 0;
};

void addCounter(std::atomic<uint64_t>& counter, uint64_t value = 1) noexcept {
    const uint64_t previous = counter.fetch_add(value, std::memory_order_relaxed);
    if (previous > std::numeric_limits<uint64_t>::max() - value) {
        counter.store(std::numeric_limits<uint64_t>::max(), std::memory_order_relaxed);
    }
}

UdpStatsSnapshot snapshotStats() {
    return {
        .client_sent = g_client_sent.load(std::memory_order_relaxed),
        .client_received = g_client_received.load(std::memory_order_relaxed),
        .client_bytes_sent = g_client_bytes_sent.load(std::memory_order_relaxed),
        .client_bytes_received = g_client_bytes_received.load(std::memory_order_relaxed),
        .server_received = g_server_received.load(std::memory_order_relaxed),
        .server_sent = g_server_sent.load(std::memory_order_relaxed),
        .server_bytes_received = g_server_bytes_received.load(std::memory_order_relaxed),
        .server_bytes_sent = g_server_bytes_sent.load(std::memory_order_relaxed),
    };
}

// UDP Echo服务器工作协程 - 多协程并发处理
Task<void> udpServerWorker(int worker_id) {
    auto socket_result = AsyncUdpSocket::create();
    if (!socket_result) {
        addCounter(g_errors);
        if (g_server_completion) {
            g_server_completion->arrive();
        }
        co_return;
    }
    AsyncUdpSocket socket = std::move(*socket_result);

    const auto reuse_addr = socket.option().handleReuseAddr();
    const auto reuse_port = socket.option().handleReusePort();
    const auto non_block = socket.option().handleNonBlock();
    if (!reuse_addr || !reuse_port || !non_block) {
        addCounter(g_errors);
        if (g_server_completion) {
            g_server_completion->arrive();
        }
        co_return;
    }

    // 设置接收缓冲区大小
    int recv_buf_size = 8 * 1024 * 1024; // 8MB
    if (setsockopt(socket.handle().fd, SOL_SOCKET, SO_RCVBUF,
                   &recv_buf_size, sizeof(recv_buf_size)) != 0) {
        addCounter(g_errors);
        if (g_server_completion) {
            g_server_completion->arrive();
        }
        co_return;
    }

    Host bindHost(IPType::IPV4, "127.0.0.1", 9090);
    auto bindResult = socket.bind(bindHost);
    if (!bindResult) {
        LogError("Worker {}: Failed to bind: {}", worker_id, bindResult.error().message());
        addCounter(g_errors);
        if (g_server_completion) {
            g_server_completion->arrive();
        }
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
        auto recvResult = co_await socket.recvfrom(buffer, sizeof(buffer), &from)
                                .timeout(SERVER_RECV_TIMEOUT);

        if (!recvResult) {
            if (IOError::contains(recvResult.error().code(), kTimeout)) {
                continue;
            }
            addCounter(g_errors);
            break;
        }

        size_t bytes = recvResult.value();
        addCounter(g_server_received);
        addCounter(g_server_bytes_received, bytes);

        // Echo回发送方
        auto sendResult = co_await socket.sendto(buffer, bytes, from);
        if (!sendResult || sendResult.value() != bytes) {
            addCounter(g_errors);
            continue;
        }
        addCounter(g_server_sent);
        addCounter(g_server_bytes_sent, sendResult.value());
    }

    const auto closed = co_await socket.close();
    if (!closed) {
        addCounter(g_errors);
    }
    if (g_server_completion) {
        g_server_completion->arrive();
    }
    co_return;
}

// UDP客户端协程 - 流水线模式
Task<void> udpBenchmarkClient(int client_id) {
    auto socket_result = AsyncUdpSocket::create();
    if (!socket_result) {
        addCounter(g_errors);
        if (g_client_completion) {
            g_client_completion->arrive();
        }
        co_return;
    }
    AsyncUdpSocket socket = std::move(*socket_result);

    const auto non_block = socket.option().handleNonBlock();
    if (!non_block) {
        addCounter(g_errors);
        if (g_client_completion) {
            g_client_completion->arrive();
        }
        co_return;
    }

    // 设置发送缓冲区大小
    int send_buf_size = 2 * 1024 * 1024; // 2MB
    if (setsockopt(socket.handle().fd, SOL_SOCKET, SO_SNDBUF,
                   &send_buf_size, sizeof(send_buf_size)) != 0) {
        addCounter(g_errors);
        if (g_client_completion) {
            g_client_completion->arrive();
        }
        co_return;
    }

    Host serverHost(IPType::IPV4, "127.0.0.1", 9090);

    // 准备测试数据
    std::vector<char> message(MESSAGE_SIZE);
    const int message_length = snprintf(
        message.data(), message.size(), "Client-%d-Message", client_id);
    if (message_length < 0 || static_cast<size_t>(message_length) >= message.size()) {
        addCounter(g_errors);
        if (g_client_completion) {
            g_client_completion->arrive();
        }
        co_return;
    }

    char recv_buffer[65536];
    uint64_t local_sent = 0;
    uint64_t local_received = 0;

    // 流水线模式：先发送一批，再接收一批
    constexpr int PIPELINE_SIZE = 10;  // 流水线深度

    while (g_running.load(std::memory_order_relaxed)) {
        // 批量发送
        for (int i = 0; i < PIPELINE_SIZE; ++i) {
            if (!g_running.load(std::memory_order_relaxed)) {
                break;
            }
            auto sendResult = co_await socket.sendto(message.data(), MESSAGE_SIZE, serverHost);
            if (!sendResult || sendResult.value() != MESSAGE_SIZE) {
                addCounter(g_errors);
                continue;
            }
            ++local_sent;
            addCounter(g_client_sent);
            addCounter(g_client_bytes_sent, sendResult.value());
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
                ++local_received;
                addCounter(g_client_received);
                addCounter(g_client_bytes_received, recvResult.value());
            } else if (!IOError::contains(recvResult.error().code(), kTimeout)) {
                addCounter(g_errors);
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
            if (!IOError::contains(recvResult.error().code(), kTimeout)) {
                addCounter(g_errors);
            }
            break;
        }
        ++local_received;
        addCounter(g_client_received);
        addCounter(g_client_bytes_received, recvResult.value());
    }

    const auto closed = co_await socket.close();
    if (!closed) {
        addCounter(g_errors);
    }
    if (g_client_completion) {
        g_client_completion->arrive();
    }
    co_return;
}

void printBenchmarkResults(std::chrono::steady_clock::time_point measurement_start,
                           std::chrono::steady_clock::time_point measurement_end,
                           const UdpStatsSnapshot& measured,
                           const UdpStatsSnapshot& settled) {
    const auto duration = std::chrono::duration_cast<std::chrono::microseconds>(
        measurement_end - measurement_start).count();
    const double duration_sec = static_cast<double>(duration) / 1'000'000.0;

    LogInfo("\n========== UDP Benchmark Results (Optimized) ==========");
    LogInfo("Test Duration: {:.2f} seconds", duration_sec);
    LogInfo("Concurrent Clients: {}", NUM_CLIENTS);
    LogInfo("Server Workers: {}", NUM_SERVER_WORKERS);
    LogInfo("Message Size: {} bytes", MESSAGE_SIZE);
    LogInfo("");
    LogInfo("Measurement window client: sent={} ({:.2f} pkt/s, {:.2f} MB/s), received={} ({:.2f} pkt/s, {:.2f} MB/s)",
            measured.client_sent,
            measured.client_sent / duration_sec,
            measured.client_bytes_sent / duration_sec / 1024.0 / 1024.0,
            measured.client_received,
            measured.client_received / duration_sec,
            measured.client_bytes_received / duration_sec / 1024.0 / 1024.0);
    LogInfo("Measurement window server: received={} ({:.2f} pkt/s, {:.2f} MB/s), sent={} ({:.2f} pkt/s, {:.2f} MB/s)",
            measured.server_received,
            measured.server_received / duration_sec,
            measured.server_bytes_received / duration_sec / 1024.0 / 1024.0,
            measured.server_sent,
            measured.server_sent / duration_sec,
            measured.server_bytes_sent / duration_sec / 1024.0 / 1024.0);
    LogInfo("Settled echo replies: sent={}, received={}, loss={:.3f}%",
            settled.client_sent,
            settled.client_received,
            settled.client_sent > 0
                ? (1.0 - static_cast<double>(settled.client_received) /
                             static_cast<double>(settled.client_sent)) * 100.0
                : 100.0);
    LogInfo("Errors: {}", g_errors.load(std::memory_order_relaxed));
    LogInfo("=======================================================\n");
}

int main() {
    LogInfo("UDP Socket Benchmark Test (Optimized)");
    LogInfo("Configuration: {} clients, {} workers, {} bytes/message, {} seconds",
            NUM_CLIENTS, NUM_SERVER_WORKERS, MESSAGE_SIZE, TEST_DURATION_SEC);

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

    const auto started = scheduler.start();
    if (!started) {
        LogError("Scheduler failed to start: {}", started.error().message());
        return 1;
    }
    LogInfo("Scheduler started");

    g_running.store(true, std::memory_order_relaxed);
    g_client_sent.store(0, std::memory_order_relaxed);
    g_client_received.store(0, std::memory_order_relaxed);
    g_client_bytes_sent.store(0, std::memory_order_relaxed);
    g_client_bytes_received.store(0, std::memory_order_relaxed);
    g_server_received.store(0, std::memory_order_relaxed);
    g_server_sent.store(0, std::memory_order_relaxed);
    g_server_bytes_received.store(0, std::memory_order_relaxed);
    g_server_bytes_sent.store(0, std::memory_order_relaxed);
    g_errors.store(0, std::memory_order_relaxed);
    g_server_workers_ready.store(0, std::memory_order_relaxed);
    g_server_ready.store(false, std::memory_order_relaxed);
    galay::benchmark::CompletionLatch client_completion(NUM_CLIENTS);
    galay::benchmark::CompletionLatch server_completion(NUM_SERVER_WORKERS);
    g_client_completion = &client_completion;
    g_server_completion = &server_completion;

    // 启动多个服务器工作协程
    for (int i = 0; i < NUM_SERVER_WORKERS; ++i) {
        if (!scheduleTask(scheduler, udpServerWorker(i))) {
            addCounter(g_errors);
            server_completion.arrive();
        }
    }
    LogInfo("Started {} server workers", NUM_SERVER_WORKERS);

    if (!galay::benchmark::waitForFlag(g_server_ready, std::chrono::seconds(2))) {
        LogError("Server workers did not become ready before client start");
        g_running.store(false, std::memory_order_relaxed);
        const bool servers_stopped = server_completion.waitFor(std::chrono::seconds(2));
        if (!servers_stopped) {
            LogError("Server workers did not stop before shutdown");
        }
        scheduler.stop();
        return 1;
    }

    // 启动多个客户端
    LogInfo("Starting {} clients...", NUM_CLIENTS);
    const auto measurement_start = std::chrono::steady_clock::now();
    for (int i = 0; i < NUM_CLIENTS; ++i) {
        if (!scheduleTask(scheduler, udpBenchmarkClient(i))) {
            addCounter(g_errors);
            client_completion.arrive();
        }
    }

    // 运行测试
    LogInfo("Benchmark running for {} seconds...", TEST_DURATION_SEC);
    std::this_thread::sleep_for(std::chrono::seconds(TEST_DURATION_SEC));

    const auto measurement_end = std::chrono::steady_clock::now();
    const auto measured = snapshotStats();
    g_running.store(false, std::memory_order_relaxed);
    const bool clients_completed = client_completion.waitFor(std::chrono::seconds(3));
    const bool servers_completed = server_completion.waitFor(std::chrono::seconds(3));

    scheduler.stop();
    LogInfo("Scheduler stopped");

    const auto settled = snapshotStats();
    printBenchmarkResults(measurement_start, measurement_end, measured, settled);

    g_client_completion = nullptr;
    g_server_completion = nullptr;
    if (!clients_completed || !servers_completed ||
        g_errors.load(std::memory_order_relaxed) != 0) {
        return 1;
    }
    return 0;
}
